#include "PatternTritonGPUOpToLLVM.h"
#include "Utility.h"
#include "triton/Dialect/TritonGEN/IR/TritonGENDialect.h"

namespace {

using namespace mlir;
using namespace mlir::triton;

// The input print op contains:
//  - a "prefix" (string) specified by the user, and
//  - one or more "operands" (tensors).
//
// For each operand, we print all of the values contained in this GPU thread,
// one per line, along with the index of the value in its tensor.
struct PrintOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::PrintOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::PrintOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::PrintOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto typeConverter = getTypeConverter();
    auto loc = op->getLoc();
    Value prefixStr = LLVM::utils::addStringToModule(
        loc, rewriter, "printfPrefix_", op.getPrefix(),
        TritonGEN::TritonGENMemorySpace::kUniformConstant);

    auto getPid = [&](int axis) {
      return mlir::LLVM::utils::llGetPid(loc, rewriter,
                                         op->getParentOfType<ModuleOp>(), axis);
    };
    std::array<Value, 3> pid = {getPid(0), getPid(1), getPid(2)};

    // Simple printf of a string without any tensors.
    if (op.getNumOperands() == 0) {
      std::string formatStr;
      llvm::raw_string_ostream os(formatStr);
      os << "pid (" << getFormatSubstr(pid[0]) << ", "
         << getFormatSubstr(pid[1]) << ", " << getFormatSubstr(pid[2]) << ")%s";
      mlir::LLVM::utils::llPrintf(rewriter, formatStr,
                                  {pid[0], pid[1], pid[2], prefixStr});
    } else {
      for (size_t i = 0; i < op.getNumOperands(); i++) {
        // Elements of the tensor that are resident in this GPU thread.
        auto elems = unpackLLElements(loc, adaptor.getOperands()[i], rewriter);

        // Get the indices of `elems` within the tensor.  Note that if `elems`
        // has an "interesting" layout, then these will not be in any
        // particularly nice order.

        // Extract the shape of the tensor being printed and use it to figure
        // out how many digits we need for each of the dimensions.
        SmallVector<int, 8> dimWidths;
        SmallVector<SmallVector<Value>> indices;
        if (auto rankedTy =
                op.getOperand(i).getType().dyn_cast<RankedTensorType>()) {
          indices = emitIndices(loc, rewriter, rankedTy.getEncoding(), rankedTy,
                                true);
          for (int64_t dim : rankedTy.getShape()) {
            if (dim > 0) {
              dimWidths.push_back(static_cast<int>(std::ceil(std::log10(dim))));
            } else {
              dimWidths.push_back(0);
            }
          }
        } else {
          // We're printing a scalar.
          assert(elems.size() == 1);
          indices.push_back({});
        }

        if (!elems.empty()) {
          printTensor(prefixStr, /*operand=*/i,
                      /*numOperands=*/op.getNumOperands(), elems, pid, indices,
                      dimWidths, op.getHex(), rewriter);
        }
      }
    }
    rewriter.eraseOp(op);
    return success();
  }

  void printTensor(Value prefixStr, size_t operand, size_t numOperands,
                   ArrayRef<Value> elems, std::array<Value, 3> pid,
                   ArrayRef<SmallVector<Value>> indices,
                   ArrayRef<int> dimWidths, bool hex,
                   ConversionPatternRewriter &rewriter) const {
    assert(!elems.empty());
    assert(elems.size() == indices.size());
    assert(dimWidths.size() == indices.front().size());

    size_t rank = dimWidths.size();

    // Format is:
    //   pid (<x>, <y>, <z>) idx (<i1>, <i2>, ...)<prefix> (operand <n>) <elem>
    // where we leave off "(operand <n>)" if there's only one operand.
    //
    // The Python wrapper munges `prefix` so that it prints nicely (e.g. starts
    // with " " and ends with ": ").

    Value formatStrValue;
    for (int i = 0; i < elems.size(); i++) {
      std::string formatStr;
      llvm::raw_string_ostream os(formatStr);

      // nvptx printf can only accept 32 args; if we pass more than that, it
      // will print garbage for the trailing args.
      constexpr int kMaxPrintfOperands = 32;
      SmallVector<Value, kMaxPrintfOperands> printfOperands;

      // TODO(jlebar): We really should pad the pid, but because the max pid is
      // not known at compile-time, this would require nontrivial device-side
      // work.
      os << "pid (";
      for (int j = 0; j < pid.size(); j++) {
        if (j != 0) {
          os << ", ";
        }
        os << getFormatSubstr(pid[j]);
        printfOperands.push_back(pid[j]);
      }
      os << ") ";

      // If `rank` is large enough, we could end up exceeding
      // kMaxPrintfOperands.  In that case, just truncate the index.
      // (Subtract 2 because we're going to add two operands after the index.)
      int maxAllowedRank = kMaxPrintfOperands - printfOperands.size() - 2;

      os << "idx (";
      const auto &index = indices[i];
      for (size_t dim = 0; dim < index.size(); dim++) {
        if (dim != 0) {
          os << ", ";
        }
        if (dim == maxAllowedRank) {
          os << "... (truncated)";
          break;
        }
        os << getFormatSubstr(index[dim], /*hex=*/false,
                              /*width=*/dimWidths[dim]);
        printfOperands.push_back(index[dim]);
      }
      os << ")";

      os << "%s";
      printfOperands.push_back(prefixStr);

      if (numOperands > 1) {
        os << "(operand " << operand << ") ";
      }

      auto elem = elems[i];
      os << getFormatSubstr(elem, hex);
      printfOperands.push_back(elem);

      // It's the same format string each iteration, but it's a lot easier if we
      // construct the format string at the same time as we populate
      // printfOperands.  But we don't want to create BLOCK_SIZE duplicate
      // strings, so we cache the Value.
      if (i == 0) {
        formatStrValue =
            mlir::LLVM::utils::llPrintf(rewriter, formatStr, printfOperands);
      } else {
        mlir::LLVM::utils::llPrintf(rewriter, formatStrValue, printfOperands);
      }
    }
  }

  std::string getFormatSubstr(Value value, bool hex = false,
                              std::optional<int> width = std::nullopt) const {
    Type type = value.getType();
    if (type.isa<LLVM::LLVMPointerType>()) {
      return "%p";
    }

    // Hex is "0x%0nx" or "0x%0nllx", where n is the number of hex digits in the
    // type (so 4 for fp16, 8 for int32, 16 for int64).
    int typeBits = type.getIntOrFloatBitWidth();
    if (hex) {
      // Ignore `width` for `hex` values, pad to typeWidth.
      std::string ret =
          "0x%0" + std::to_string(type.getIntOrFloatBitWidth() / 4);
      if (type.getIntOrFloatBitWidth() > 32) {
        ret += "ll";
      }
      ret += "x";
      return ret;
    }

    std::string prefix = "%";
    if (width.has_value()) {
      prefix += std::to_string(*width);
    } else if (hex) {
      prefix += "0";
      prefix += std::to_string(value.getType().getIntOrFloatBitWidth() / 4);
    }

    if (type.isa<LLVM::LLVMPointerType>()) {
      return prefix + "p";
    } else if (type.isBF16() || type.isF16() || type.isF32() || type.isF64()) {
      return prefix + "f";
    } else if (type.isSignedInteger()) {
      if (type.getIntOrFloatBitWidth() == 64)
        return prefix + "lli";
      else
        return prefix + "i";
    } else if (type.isUnsignedInteger() || type.isSignlessInteger()) {
      if (type.getIntOrFloatBitWidth() == 64)
        return prefix + "llu";
      else
        return prefix + "u";
    }
    assert(false && "not supported type");
    return "";
  }
};

} // namespace

void mlir::triton::intel::populatePrintOpToLLVMPattern(
    TritonGPUToLLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    PatternBenefit benefit) {
  patterns.add<PrintOpConversion>(typeConverter, benefit);
}