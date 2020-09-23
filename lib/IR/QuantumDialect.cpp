/* Place additional code not defined by the ODS or RRD systems here,
   e.g. operations, custom types, attributes etc. */

#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/AsmState.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/StringSwitch.h"

#include "QuantumDialect.h"

#include <sstream>

using namespace mlir;
using namespace mlir::quantum;

#define EMIT_ERROR(p, m) p.emitError(p.getCurrentLocation(), m)


//===------------------------------------------------------------------------------------------===//
// Dialect Definitions
//===------------------------------------------------------------------------------------------===//

// latest changes in MLIR upstream now only require this function for op, type, etc. registration
void QuantumDialect::initialize() {
    addOperations<
        #define GET_OP_LIST
        #include "QuantumOps.cpp.inc"
    >();

    addTypes<QubitType, QuregType, OpType, COpType, CircType>();
}

namespace mlir {
namespace quantum {
namespace detail {
// This class represents the internal storage of the Quantum 'QuregType'.
struct QuregTypeStorage : public TypeStorage {
    // The `KeyTy` is a required type that provides an interface for the storage instance.
    // This type will be used when uniquing an instance of the type storage. For our Qureg
    // type, we will unique each instance on its size.
    using KeyTy = int;

    // Size of the qubit register
    llvm::Optional<int> size;

    // A constructor for the type storage instance.
    QuregTypeStorage(llvm::Optional<int> size) {
        assert(!size || *size > 1 && "Register type must have size > 1!");
        this->size = size;
    }

    // Define the comparison function for the key type with the current storage instance.
    // This is used when constructing a new instance to ensure that we haven't already
    // uniqued an instance of the given key.
    bool operator==(const KeyTy &key) const {
        return size == (key < 0 ? llvm::None : llvm::Optional<int>(key));
    }

    // Define a construction method for creating a new instance of this storage.
    // This method takes an instance of a storage allocator, and an instance of a `KeyTy`.
    // The given allocator must be used for *all* necessary dynamic allocations used to
    // create the type storage and its internal.
    static QuregTypeStorage *construct(TypeStorageAllocator &allocator, const KeyTy &key) {
        // Allocate the storage instance and construct it.
        llvm::Optional<int> size = key < 0 ? llvm::None : llvm::Optional<int>(key);
        return new (allocator.allocate<QuregTypeStorage>()) QuregTypeStorage(size);
    }
};

// This class represents the internal storage of the Quantum 'COpType'.
struct COpTypeStorage : public TypeStorage {
    using KeyTy = std::pair<unsigned, Type>;

    unsigned nctrl;
    Type baseType;

    COpTypeStorage(unsigned nctrl, Type baseType) {
        assert(nctrl > 0 && "Number of controls must be > 0");
        if (baseType)
            assert(baseType.isa<OpType>() || baseType.isa<CircType>() &&
                   "Base type of controlled op can only be supported quantum operations!");
        this->nctrl = nctrl;
        this->baseType = baseType;
    }

    bool operator==(const KeyTy &key) const { return key.first == nctrl && key.second == baseType; }

    static COpTypeStorage *construct(TypeStorageAllocator &allocator, const KeyTy &key) {
        return new (allocator.allocate<COpTypeStorage>()) COpTypeStorage(key.first, key.second);
    }
};
} // end namespace detail
} // end namespace quantum
} // end namespace mlir


//===------------------------------------------------------------------------------------------===//
// Method implementations of complex types
//===------------------------------------------------------------------------------------------===//

// Qureg
QuregType QuregType::get(MLIRContext *ctx, llvm::Optional<int> size) {
    // Parameters to the storage class are passed after the custom type kind.
    detail::QuregTypeStorage::KeyTy key = size ? *size : -1;
    return Base::get(ctx, key);
}

llvm::Optional<int> QuregType::getNumQubits() {
    // 'getImpl' returns a pointer to our internal storage instance.
    return getImpl()->size;
}

// COp
COpType COpType::get(MLIRContext *ctx, unsigned nctrl, Type baseType) {
    // Parameters to the storage class are passed after the custom type kind.
    return Base::get(ctx, nctrl, baseType);
}

unsigned COpType::getNumCtrls() {
    // 'getImpl' returns a pointer to our internal storage instance.
    return getImpl()->nctrl;
}

Type COpType::getBaseType() {
    // 'getImpl' returns a pointer to our internal storage instance.
    return getImpl()->baseType;
}


//===------------------------------------------------------------------------------------------===//
// Dialect types printing and parsing
//===------------------------------------------------------------------------------------------===//

// Print an instance of a type registered in the Quantum dialect.
void QuantumDialect::printType(Type type, DialectAsmPrinter &printer) const {
    // Differentiate between the Quantum types and print accordingly.
    llvm::TypeSwitch<Type>(type)
        .Case<QubitType>([&](QubitType)   { printer << "qubit"; })
        .Case<QuregType>([&](QuregType t) { printer << "qureg<";
                                            if (auto numQubits = t.getNumQubits())
                                                printer << *numQubits;
                                            printer << ">"; })
        .Case<OpType>   ([&](OpType)      { printer << "op"; })
        .Case<COpType>  ([&](COpType t)   { printer << "cop<" << t.getNumCtrls();
                                            if (auto baseType = t.getBaseType())
                                                printer << ", " << baseType;
                                            printer << ">"; })
        .Case<CircType> ([&](CircType)    { printer << "circ"; })
        .Default([](Type) { llvm_unreachable("unrecognized type encountered in the printer!"); });
}

// Parse an instance of a type registered to the Quantum dialect.
Type QuantumDialect::parseType(DialectAsmParser &parser) const {
    // NOTE: All MLIR parser function return a ParseResult. This is a
    // specialization of LogicalResult that auto-converts to a `true` boolean
    // value on failure to allow for chaining, but may be used with explicit
    // `mlir::failed/mlir::succeeded` as desired.
    Builder &builder = parser.getBuilder();

    // Attempt to parse all supported dialect types.
    StringRef keyword;
    if (parser.parseKeyword(&keyword))
        return EMIT_ERROR(parser, "error parsing type keyword!"), nullptr;

    // Lambdas are needed so as not to call the helper functions due to eager parameter evaluation
    Type result = llvm::StringSwitch<function_ref<Type()>>(keyword)
        .Case("qubit", [&] { return builder.getType<QubitType>(); })
        .Case("qureg", [&] { return parseQuregType(parser); })
        .Case("op",    [&] { return builder.getType<OpType>(); })
        .Case("cop",   [&] { return parseCOpType(parser); })
        .Case("circ",  [&] { return builder.getType<CircType>(); })
        .Default([&] { return EMIT_ERROR(parser, "unrecognized quantum type!"), nullptr; })();

    return result;
}

Type QuantumDialect::parseQuregType(DialectAsmParser &parser) {
    StringRef errmsg = "error during 'Qureg' type parsing!";
    llvm::Optional<int> optionalSize;
    int size;

    if (parser.parseLess())
        return EMIT_ERROR(parser, errmsg), nullptr;

    auto res = parser.parseOptionalInteger<int>(size);
    if (res.hasValue() && failed(res.getValue()))
        return EMIT_ERROR(parser, errmsg), nullptr;
    optionalSize = res.hasValue() ? llvm::Optional<int>(size) : llvm::None;

    if (parser.parseGreater())
        return EMIT_ERROR(parser, errmsg), nullptr;

    return parser.getBuilder().getType<QuregType>(optionalSize);
}

Type QuantumDialect::parseCOpType(DialectAsmParser &parser) {
    StringRef errmsg = "error during 'COp' type parsing!";
    Type baseType(nullptr);
    int nctrl;

    if (parser.parseLess() || parser.parseInteger<int>(nctrl))
        return EMIT_ERROR(parser, errmsg), nullptr;

    if (succeeded(parser.parseOptionalComma()))
        if (parser.parseType(baseType))
            return EMIT_ERROR(parser, errmsg), nullptr;

    if (parser.parseGreater())
        return EMIT_ERROR(parser, errmsg), nullptr;

    if (baseType && !(baseType.isa<OpType>() || baseType.isa<CircType>()))
        return EMIT_ERROR(parser, "Base type of controlled op must be either 'Op' or 'Circ' type!"),
               nullptr;

    return parser.getBuilder().getType<COpType>(nctrl, baseType);
}


//===------------------------------------------------------------------------------------------===//
// Static parse helper methods for the register access interface
//===------------------------------------------------------------------------------------------===//

// Parse a list of register range indeces (accessors) that can be either SSA values of type Index
// or some constant integer attribute.
static ParseResult parseOperandOrIntAttrList(OpAsmParser &parser, OperationState &result,
        Builder &builder, ArrayAttr &accessors, int64_t dynVal,
        SmallVectorImpl<OpAsmParser::OperandType> &ssa) {
    if (failed(parser.parseOptionalLSquare())) {
        accessors = builder.getI64ArrayAttr({});
        return success();
    }

    // there are atmost 3 range accessors to parse: start, size, step
    SmallVector<int64_t, 3> attrVals;
    for (unsigned i = 0; i < 3; i++) {
        OpAsmParser::OperandType operand;
        auto res = parser.parseOptionalOperand(operand);
        if (res.hasValue() && succeeded(res.getValue())) {
            ssa.push_back(operand);
            attrVals.push_back(dynVal);
        } else {
            Attribute attr;
            NamedAttrList placeholder;
            if (failed(parser.parseAttribute(attr, "_", placeholder)) || !attr.isa<IntegerAttr>())
                return parser.emitError(parser.getNameLoc()) << "expected SSA value or integer";
            attrVals.push_back(attr.cast<IntegerAttr>().getInt());
        }

        if (succeeded(parser.parseOptionalComma()))
            continue;
        if (failed(parser.parseRSquare()))
            return failure();
        else
            break;
    }

    accessors = builder.getI64ArrayAttr(attrVals);
    return success();
}

// Parse any operands and add them to the allOperands list. If any of them are of qureg type,
// additionally try to parse a register accessor list. For every such operand, we also need to
// populate the corresponding array attribute that specifies how many (if any) accessors are
// constants. Get the attribute names from the interface method.
// Also populate the 'getOperandSegmentSizeAttr' for multiple variadic operands ops.
template<typename OpClass>
static ParseResult parseOperandListWithAccessors(OpAsmParser &parser, OperationState &result,
        SmallVectorImpl<OpAsmParser::OperandType> &allOperands,
        SmallVectorImpl<Type> &allOperandTypes, unsigned &parsed) {
    Builder b = parser.getBuilder();
    SmallVector<int, 5> segmentSizes(OpClass::getSegmentSizesArraySize(), 0);
    ArrayRef<bool> isRegLike;
    unsigned numRegLike, currRegLike = 0;
    std::tie(isRegLike, numRegLike) = OpClass::getRegLikeArray();

    unsigned totalParsed = 0; parsed = 0;
    do {
        OpAsmParser::OperandType currentOperand;
        auto res = parser.parseOptionalOperand(currentOperand);
        if (res.hasValue()){
            if (failed(*res))
                return failure();

            allOperands.push_back(currentOperand);
            allOperandTypes.push_back(nullptr);
            segmentSizes[totalParsed++] = 1;

            if (isRegLike[parsed]) {
                SmallVector<OpAsmParser::OperandType, 3> ssaAccessors;
                ArrayAttr staticAccessors;
                if (parseOperandOrIntAttrList(parser, result, b, staticAccessors,
                                              ShapedType::kDynamicSize, ssaAccessors))
                    return failure();
                // populates the static range attribute array
                result.addAttribute(OpClass::getAccessorAttrName(currRegLike++), staticAccessors);
                allOperands.append(ssaAccessors.begin(), ssaAccessors.end());
                allOperandTypes.append(ssaAccessors.size(), b.getIndexType());
                segmentSizes[totalParsed++] = ssaAccessors.size();
            }

            parsed++;
        }
    } while (succeeded(parser.parseOptionalComma()));
    // fill the static accessor array attributes for non-parsed operands with empty arrays
    while (currRegLike < numRegLike)
        result.addAttribute(OpClass::getAccessorAttrName(currRegLike++), b.getI64ArrayAttr({}));
    result.addAttribute(OpClass::getOperandSegmentSizeAttr(), b.getI32VectorAttr(segmentSizes));
    return success();
}

// This is a variation of the 'parseOperandListWithAccessors' function which parses a mixed list
// of operands of which some have register accessors (and one of which could be optional).
// In contrast, this function only parses two operands, but both of which are variadic:
// a number of QData values, as well as accompanying accessors.
static ParseResult parseVariadicOperandWithAccessors(OpAsmParser &parser, OperationState &result,
        SmallVectorImpl<OpAsmParser::OperandType> &varOperands,
        SmallVectorImpl<OpAsmParser::OperandType> &varAccessors,
        SmallVectorImpl<Attribute> &allStaticAccessors, int &parsedOperands, int &parsedAccessors) {
    Builder b = parser.getBuilder();

    parsedOperands = parsedAccessors = 0;
    do {
        OpAsmParser::OperandType currentOperand;
        auto res = parser.parseOptionalOperand(currentOperand);
        if (res.hasValue()){
            if (failed(*res))
                return failure();

            varOperands.push_back(currentOperand);
            parsedOperands++;

            SmallVector<OpAsmParser::OperandType, 3> ssaAccessors;
            ArrayAttr staticAccessors;
            if (parseOperandOrIntAttrList(parser, result, b, staticAccessors,
                                          ShapedType::kDynamicSize, ssaAccessors))
                return failure();
            varAccessors.append(ssaAccessors.begin(), ssaAccessors.end());
            allStaticAccessors.push_back(staticAccessors);
            parsedAccessors += ssaAccessors.size();
        } else {
            break;
        }
    } while (succeeded(parser.parseOptionalComma()));

    return success();
}


//===------------------------------------------------------------------------------------------===//
// Custom assembly format for quantum operations
//===------------------------------------------------------------------------------------------===//

// Templated print function that handles all ops with the register access interface
template<typename OpClass> static void print(OpAsmPrinter &p, OpClass op) {
    SmallVector<StringRef, 3> elidedAttrs;
    p << op.getOperationName();

    // In case of the Rz gate, print the angle parameter
    if (std::is_same<OpClass, RzOp>::value) {
        p << "(";
        p.printAttributeWithoutType(op.getAttrOfType<FloatAttr>("phi"));
        p << ")";
        elidedAttrs.push_back("phi");
    }

    // print all operands, including any register indeces in brackets
    for (unsigned totIdx = 0, argIdx = 0, reglikeIdx = 0; totIdx < op.getOperands().size();) {
        p << " ";
        p.printOperand(op.getOperand(totIdx++));

        if (op.getRegLikeArray().first[argIdx++]) {
            ArrayAttr array = op.getAttrOfType<ArrayAttr>(op.getAccessorAttrName(reglikeIdx++));
            if (array.size())
                p << "[";
            const char *sep = "";
            for (auto attr : array) {
                p << sep;
                if (attr.dyn_cast<IntegerAttr>().getInt() == -1)
                    p.printOperand(op.getOperand(totIdx++));
                else
                    p.printAttributeWithoutType(attr);
                sep = ", ";
            }
            if (array.size())
                p << "]";
        }
    }

    // print the attribute dictionary excluding any attributes used by the register access interface
    elidedAttrs.push_back(op.getOperandSegmentSizeAttr());
    for (auto attrName : op.getAccessorAttrNames())
        elidedAttrs.push_back(attrName);
    p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/elidedAttrs);

    // print the operand types except those that are register indeces
    const char *sep = "";
    if (op.getOperands().size())
        p << " : ";
    for (unsigned totIdx = 0, argIdx = 0, reglikeIdx = 0; totIdx < op.getOperands().size();) {
        p << sep;
        p.printType(op.getOperand(totIdx++).getType());
        if (op.getRegLikeArray().first[argIdx++]) {
            ArrayAttr array = op.getAttrOfType<ArrayAttr>(op.getAccessorAttrName(reglikeIdx++));
            for (auto attr : array) {
                // loop past the accessor operands
                if (attr.dyn_cast<IntegerAttr>().getInt() == -1)
                    totIdx++;
            }
        }
        sep = ", ";
    }

    // print all result types
    p.printOptionalArrowTypeList(op.getResultTypes());
}

// print function for the parametric circuit op
static void print(OpAsmPrinter &p, ParametricCircuitOp op) {
    SmallVector<StringRef, 3> elidedAttrs;
    ArrayAttr array = op.getAttrOfType<ArrayAttr>(op.getAccessorAttrName(0));
    p << op.getOperationName() << " ";

    p.printAttributeWithoutType(op.calleeAttr());
    p << "(";
    p.printAttributeWithoutType(op.nAttr());

    for (unsigned argIdx = 0, rangeIdx = 0; argIdx < op.qbs().size(); argIdx++) {
        p << ", ";
        p.printOperand(op.qbs()[argIdx]);
        if (array.size()) {
            ArrayAttr subArray = array[argIdx].dyn_cast<ArrayAttr>();
            if (subArray.size())
                p << "[";
            const char *sep = "";
            for (auto attr : subArray) {
                p << sep;
                if (attr.dyn_cast<IntegerAttr>().getInt() == -1)
                    p.printOperand(op.ranges()[rangeIdx++]);
                else
                    p.printAttributeWithoutType(attr);
                sep = ", ";
            }
            if (subArray.size())
                p << "]";
        }
    }

    p << ")";

    elidedAttrs.push_back(op.getOperandSegmentSizeAttr());
    elidedAttrs.push_back(op.getAccessorAttrName(0));
    elidedAttrs.push_back("callee");
    elidedAttrs.push_back("n");
    p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/elidedAttrs);

    p << " : ";
    llvm::interleaveComma(op.qbs().getTypes(), p);

    p << " -> ";
    p.printType(op.getResult().getType());
}

// This parse function can be used with all quantum ops that implement the RegAccessInterface.
// It parses all available operands and their types in the pretty parse format of quantum gates,
// with the addition of allowing a list of accessors indeces (Index value OR integer constant)
// for each of the operands of type 'qureg'. Note that the type of the indeces (if given as SSA
// value) must not be specified, and is assumed to be IndexType.
template<typename OpClass>
static ParseResult parseRegAccessOps(OpAsmParser &p, OperationState &result, bool param = false) {
    // The most operands a gate can have is: heldOp (1), ctrl (1+3), trgt (1+3) = 9
    SmallVector<OpAsmParser::OperandType, 9> allOperands;
    SmallVector<Type, 9> allOperandTypes;
    SmallVector<Type, 3> nonAccessorOperandTypes;
    SmallVector<Type, 1> allReturnTypes;
    unsigned numMissingTypes;

    if (param) {
        FloatAttr phiAttr;
        if (p.parseLParen())
            return failure();
        if (p.parseAttribute(phiAttr, "phi", result.attributes))
            return failure();
        if (p.parseRParen())
            return failure();
    }

    llvm::SMLoc allOperandLoc = p.getCurrentLocation();
    if (parseOperandListWithAccessors<OpClass>(p, result, allOperands, allOperandTypes,
                                               numMissingTypes))
        return failure();

    if (p.parseOptionalAttrDict(result.attributes))
        return failure();

    llvm::SMLoc loc = p.getCurrentLocation();
    if (succeeded(p.parseOptionalColon()) && p.parseTypeList(nonAccessorOperandTypes))
        return failure();

    if (numMissingTypes != nonAccessorOperandTypes.size())
        return p.emitError(loc, "number of provided operand types "
                                "(" + std::to_string(nonAccessorOperandTypes.size()) + ")"
                                " doesn't match expected "
                                "(" + std::to_string(numMissingTypes) + ")");

    for (unsigned i = 0, j = 0; i < allOperandTypes.size(); i++) {
        if(!allOperandTypes[i])
            allOperandTypes[i] = nonAccessorOperandTypes[j++];
    }

    if (p.resolveOperands(allOperands, allOperandTypes, allOperandLoc, result.operands))
        return failure();

    // parse optional return type
    if (succeeded(p.parseOptionalArrow())) {
        if (p.parseTypeList(allReturnTypes))
            return failure();
        result.addTypes(allReturnTypes);
    }

    return success();
}

// custom parsing for the ParametricCircuitOp
static ParseResult parseParametricCircuitOp(OpAsmParser &p, OperationState &result) {
    SmallVector<OpAsmParser::OperandType, 9> allOperands;
    SmallVector<Type, 9> allOperandTypes;
    SmallVector<Type, 1> allReturnTypes;
    SmallVector<Attribute, 3> accessorArray;
    int parsedOperands, parsedAccessors;
    Builder b = p.getBuilder();

    FlatSymbolRefAttr calleeAttr;
    if (p.parseAttribute<FlatSymbolRefAttr>(calleeAttr, "callee", result.attributes))
        return failure();
    if (p.parseLParen())
        return failure();

    IntegerAttr nAttr;
    if (p.parseAttribute(nAttr, "n", result.attributes))
        return failure();
    if (p.parseComma())
        return failure();

    // Since the variadic QData operands must come before any accessor operands, they can already
    // be loaded onto the final list (allOperands), the accessors will then be appended at the end.
    // The accessor array is populated inside the function call, which must then be added to the op.
    llvm::SMLoc allOperandLoc = p.getCurrentLocation();
    SmallVector<OpAsmParser::OperandType, 6> accessorOperands;
    if (parseVariadicOperandWithAccessors(p, result, allOperands, accessorOperands, accessorArray,
                                          parsedOperands, parsedAccessors))
        return failure();

    // now append the parsed accessor operands to the end of the list
    allOperands.append(accessorOperands.begin(), accessorOperands.end());
    // fill the static accessor array attributes for non-parsed operands with empty arrays
    result.addAttribute(ParametricCircuitOp::getAccessorAttrName(0), b.getArrayAttr(accessorArray));
    // add the segment sizes of the variadic operands
    result.addAttribute(ParametricCircuitOp::getOperandSegmentSizeAttr(),
                        b.getI32VectorAttr({parsedOperands, parsedAccessors}));

    if (p.parseRParen())
        return failure();

    if (p.parseOptionalAttrDict(result.attributes))
        return failure();

    llvm::SMLoc loc = p.getCurrentLocation();
    if (succeeded(p.parseOptionalColon()) && p.parseTypeList(allOperandTypes))
        return failure();

    if (parsedOperands != allOperandTypes.size())
        return p.emitError(loc, "number of provided operand types "
                                "(" + std::to_string(allOperandTypes.size()) + ")"
                                " doesn't match expected "
                                "(" + std::to_string(parsedOperands) + ")");

    for (unsigned i = 0; i < parsedAccessors; i++) {
        allOperandTypes.push_back(b.getIndexType());
    }

    if (p.resolveOperands(allOperands, allOperandTypes, allOperandLoc, result.operands))
        return failure();

    // parse optional return type
    if (succeeded(p.parseOptionalArrow())) {
        if (p.parseTypeList(allReturnTypes))
            return failure();
        result.addTypes(allReturnTypes);
    }

    return success();
}


//===------------------------------------------------------------------------------------------===//
// Additional implementations of OpInterface methods
//===------------------------------------------------------------------------------------------===//

// Return the callee, required by the call interface.
CallInterfaceCallable ParametricCircuitOp::getCallableForCallee() {
    return getAttrOfType<SymbolRefAttr>("callee");
}

// Get the arguments to the called function, required by the call interface.
Operation::operand_range ParametricCircuitOp::getArgOperands() {
    return qbs();
}

#define GET_INTERFACE_CLASSES
#include "QuantumInterfaces.cpp.inc"
#define GET_OP_CLASSES
#include "QuantumOps.cpp.inc"
