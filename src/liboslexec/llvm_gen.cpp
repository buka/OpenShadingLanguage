/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cmath>

#include "llvm_headers.h"

#include "oslexec_pvt.h"
#include "genclosure.h"
#include "runtimeoptimize.h"

using namespace OSL;
using namespace OSL::pvt;

#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {

static ustring op_and("and");
static ustring op_bitand("bitand");
static ustring op_bitor("bitor");
static ustring op_break("break");
static ustring op_ceil("ceil");
static ustring op_cellnoise("cellnoise");
static ustring op_color("color");
static ustring op_compl("compl");
static ustring op_continue("continue");
static ustring op_dowhile("dowhile");
static ustring op_eq("eq");
static ustring op_error("error");
static ustring op_fabs("fabs");
static ustring op_floor("floor");
static ustring op_for("for");
static ustring op_format("format");
static ustring op_ge("ge");
static ustring op_gt("gt");
static ustring op_if("if");
static ustring op_le("le");
static ustring op_lt("lt");
static ustring op_min("min");
static ustring op_neq("neq");
static ustring op_normal("normal");
static ustring op_or("or");
static ustring op_point("point");
static ustring op_printf("printf");
static ustring op_round("round");
static ustring op_shl("shl");
static ustring op_shr("shr");
static ustring op_sign("sign");
static ustring op_step("step");
static ustring op_trunc("trunc");
static ustring op_vector("vector");
static ustring op_warning("warning");
static ustring op_xor("xor");



/// Macro that defines the arguments to LLVM IR generating routines
///
#define LLVMGEN_ARGS     RuntimeOptimizer &rop, int opnum

/// Macro that defines the full declaration of an LLVM generator.
/// 
#define LLVMGEN(name)  bool name (LLVMGEN_ARGS)

// Forward decl
LLVMGEN (llvm_gen_generic);



void
RuntimeOptimizer::llvm_gen_debug_printf (const std::string &message)
{
    ustring s = ustring::format ("(%s %s) %s", inst()->shadername().c_str(),
                                 inst()->layername().c_str(), message.c_str());
    llvm::Value *args[3] = { sg_void_ptr(), llvm_constant("%s\n"),
                             llvm_constant(s) };
    llvm::Function *func = llvm_module()->getFunction ("osl_printf");
    llvm_call_function (func, args, 3);
}



void
RuntimeOptimizer::llvm_call_layer (int layer, bool unconditional)
{
    // Make code that looks like:
    //     if (! groupdata->run[parentlayer]) {
    //         groupdata->run[parentlayer] = 1;
    //         parent_layer (sg, groupdata);
    //     }
    // if it's a conditional call, or
    //     groupdata->run[parentlayer] = 1;
    //     parent_layer (sg, groupdata);
    // if it's run unconditionally.

    llvm::Value *args[2];
    args[0] = sg_ptr ();
    args[1] = groupdata_ptr ();

    ShaderInstance *parent = group()[layer];
    llvm::Value *trueval = llvm_constant_bool(true);
    llvm::Value *layerfield = layer_run_ptr(layer_remap(layer));
    llvm::BasicBlock *then_block = NULL, *after_block = NULL;
    if (! unconditional) {
        llvm::Value *executed = builder().CreateLoad (layerfield);
        executed = builder().CreateICmpNE (executed, trueval);
        then_block = llvm_new_basic_block ("");
        after_block = llvm_new_basic_block ("");
        builder().CreateCondBr (executed, then_block, after_block);
        builder().SetInsertPoint (then_block);
    }

    builder().CreateStore (trueval, layerfield);
    std::string name = Strutil::format ("%s_%d", parent->layername().c_str(),
                                        parent->id());
    // Mark the call as a fast call
    llvm::CallInst* call_inst = llvm::cast<llvm::CallInst>(llvm_call_function (name.c_str(), args, 2));
    call_inst->setCallingConv (llvm::CallingConv::Fast);

    if (! unconditional) {
        builder().CreateBr (after_block);
        builder().SetInsertPoint (after_block);
    }
}



void
RuntimeOptimizer::llvm_run_connected_layers (Symbol &sym, int symindex,
                                             std::vector<int> *already_run)
{
    if (sym.valuesource() != Symbol::ConnectedVal)
        return;  // Nothing to do

    for (int c = 0;  c < inst()->nconnections();  ++c) {
        const Connection &con (inst()->connection (c));
        // If the connection gives a value to this param
        if (con.dst.param == symindex) {
            if (already_run) {
                if (std::find (already_run->begin(), already_run->end(), con.srclayer) != already_run->end())
                    continue;  // already ran that one
                else
                    already_run->push_back (con.srclayer);  // mark it
            }

            // If the earlier layer it comes from has not yet been
            // executed, do so now.
            llvm_call_layer (con.srclayer);
        }
    }
}



LLVMGEN (llvm_gen_useparam)
{
    ASSERT (! rop.inst()->unused() &&
            "oops, thought this layer was unused, why do we call it?");

    // If we have multiple params needed on this statement, don't waste
    // time checking the same upstream layer more than once.
    std::vector<int> already_run;

    Opcode &op (rop.inst()->ops()[opnum]);
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol& sym = *rop.opargsym (op, i);
        int symindex = rop.inst()->arg (op.firstarg()+i);
        rop.llvm_run_connected_layers (sym, symindex, &already_run);
    }
    return true;
}



// Used for printf, error, warning, format
LLVMGEN (llvm_gen_printf)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    // Prepare the args for the call
    
    // Which argument is the format string?  Usually 0, but for op
    // format(), the formatting string is argument #1.
    int format_arg = (op.opname() == "format" ? 1 : 0);
    Symbol& format_sym = *rop.opargsym (op, format_arg);

    std::vector<llvm::Value*> call_args;
    if (!format_sym.is_constant()) {
        rop.shadingsys().warning ("%s must currently have constant format\n",
                                  op.opname().c_str());
        return false;
    }

    // For some ops, we push the shader globals pointer
    if (op.opname() == op_printf || op.opname() == op_error ||
            op.opname() == op_warning)
        call_args.push_back (rop.sg_void_ptr());

    // We're going to need to adjust the format string as we go, but I'd
    // like to reserve a spot for the char*.
    size_t new_format_slot = call_args.size();
    call_args.push_back(NULL);

    ustring format_ustring = *((ustring*)format_sym.data());
    const char* format = format_ustring.c_str();
    std::string s;
    int arg = format_arg + 1;
    while (*format != '\0') {
        if (*format == '%') {
            if (format[1] == '%') {
                // '%%' is a literal '%'
                s += "%%";
                format += 2;  // skip both percentages
                continue;
            }
            const char *oldfmt = format;  // mark beginning of format
            while (*format &&
                   *format != 'c' && *format != 'd' && *format != 'e' &&
                   *format != 'f' && *format != 'g' && *format != 'i' &&
                   *format != 'm' && *format != 'n' && *format != 'o' &&
                   *format != 'p' && *format != 's' && *format != 'u' &&
                   *format != 'v' && *format != 'x' && *format != 'X')
                ++format;
            ++format; // Also eat the format char
            if (arg >= op.nargs()) {
                rop.shadingsys().error ("Mismatch between format string and arguments (%s:%d)",
                                        op.sourcefile().c_str(), op.sourceline());
                return false;
            }

            std::string ourformat (oldfmt, format);  // straddle the format
            // Doctor it to fix mismatches between format and data
            Symbol& sym (*rop.opargsym (op, arg));
            TypeDesc simpletype (sym.typespec().simpletype());
            int num_elements = simpletype.numelements();
            int num_components = simpletype.aggregate;
            // NOTE(boulos): Only for debug mode do the derivatives get printed...
            for (int a = 0;  a < num_elements;  ++a) {
                llvm::Value *arrind = simpletype.arraylen ? rop.llvm_constant(a) : NULL;
                if (sym.typespec().is_closure_based()) {
                    s += ourformat;
                    llvm::Value *v = rop.llvm_load_value (sym, 0, arrind, 0);
                    v = rop.llvm_call_function ("osl_closure_to_string", rop.sg_void_ptr(), v);
                    call_args.push_back (v);
                    continue;
                }

                for (int c = 0; c < num_components; c++) {
                    if (c != 0 || a != 0)
                        s += " ";
                    s += ourformat;

                    llvm::Value* loaded = rop.llvm_load_value (sym, 0, arrind, c);
                    if (sym.typespec().is_floatbased()) {
                        // C varargs convention upconverts float->double.
                        loaded = rop.builder().CreateFPExt(loaded, llvm::Type::getDoubleTy(rop.llvm_context()));
                    }

                    call_args.push_back (loaded);
                }
            }
            ++arg;
        } else {
            // Everything else -- just copy the character and advance
            s += *format++;
        }
    }

    // Some ops prepend things
    if (op.opname() == op_error || op.opname() == op_warning) {
        std::string prefix = Strutil::format ("Shader %s [%s]: ",
                                              op.opname().c_str(),
                                              rop.inst()->shadername().c_str());
        s = prefix + s;
    }

    // Now go back and put the new format string in its place
    call_args[new_format_slot] = rop.llvm_constant (s.c_str());

    // Construct the function name and call it.
    std::string opname = std::string("osl_") + op.opname().string();
    llvm::Value *ret = rop.llvm_call_function (opname.c_str(), &call_args[0],
                                               (int)call_args.size());

    // The format op returns a string value, put in in the right spot
    if (op.opname() == op_format)
        rop.llvm_store_value (ret, *rop.opargsym (op, 0));
    return true;
}



LLVMGEN (llvm_gen_add)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    ASSERT (! A.typespec().is_array() && ! B.typespec().is_array());
    if (Result.typespec().is_closure()) {
        ASSERT (A.typespec().is_closure() && B.typespec().is_closure());
        llvm::Value *valargs[3];
        valargs[0] = rop.sg_void_ptr();
        valargs[1] = rop.llvm_load_value (A);
        valargs[2] = rop.llvm_load_value (B);
        llvm::Value *res = rop.llvm_call_function ("osl_add_closure_closure", valargs, 3);
        rop.llvm_store_value (res, Result, 0, NULL, 0);
        return true;
    }

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // The following should handle f+f, v+v, v+f, f+v, i+i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFAdd(a, b)
                                  : rop.builder().CreateAdd(a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1;  d <= 2;  ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *a = rop.loadLLVMValue (A, i, d, type);
                    llvm::Value *b = rop.loadLLVMValue (B, i, d, type);
                    llvm::Value *r = rop.builder().CreateFAdd(a, b);
                    rop.storeLLVMValue (r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_sub)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    ASSERT (! Result.typespec().is_closure_based() &&
            "subtraction of closures not supported");

    // The following should handle f-f, v-v, v-f, f-v, i-i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFSub(a, b)
                                  : rop.builder().CreateSub(a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1;  d <= 2;  ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *a = rop.loadLLVMValue (A, i, d, type);
                    llvm::Value *b = rop.loadLLVMValue (B, i, d, type);
                    llvm::Value *r = rop.builder().CreateFSub(a, b);
                    rop.storeLLVMValue (r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_mul)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = !Result.typespec().is_closure_based() && Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // multiplication involving closures
    if (Result.typespec().is_closure()) {
        llvm::Value *valargs[3];
        valargs[0] = rop.sg_void_ptr();
        bool tfloat;
        if (A.typespec().is_closure()) {
            tfloat = B.typespec().is_float();
            valargs[1] = rop.llvm_load_value (A);
            valargs[2] = tfloat ? rop.llvm_load_value (B) : rop.llvm_void_ptr(B);
        } else {
            tfloat = A.typespec().is_float();
            valargs[1] = rop.llvm_load_value (B);
            valargs[2] = tfloat ? rop.llvm_load_value (A) : rop.llvm_void_ptr(A);
        }
        llvm::Value *res = tfloat ? rop.llvm_call_function ("osl_mul_closure_float", valargs, 3)
                                  : rop.llvm_call_function ("osl_mul_closure_color", valargs, 3);
        rop.llvm_store_value (res, Result, 0, NULL, 0);
        return true;
    }

    // multiplication involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_mul_m_ff", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_mul_mf", Result, B, A);
            else ASSERT(0);
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_mul_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_mul_mm", Result, A, B);
            else ASSERT(0);
        } else ASSERT (0);
        if (Result.has_derivs())
            rop.llvm_zero_derivs (Result);
        return true;
    }

    // The following should handle f*f, v*v, v*f, f*v, i*i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.llvm_load_value (A, 0, i, type);
        llvm::Value *b = rop.llvm_load_value (B, 0, i, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFMul(a, b)
                                  : rop.builder().CreateMul(a, b);
        rop.llvm_store_value (r, Result, 0, i);

        if (Result.has_derivs() && (A.has_derivs() || B.has_derivs())) {
            // Multiplication of duals: (a*b, a*b.dx + a.dx*b, a*b.dy + a.dy*b)
            ASSERT (is_float);
            llvm::Value *ax = rop.llvm_load_value (A, 1, i, type);
            llvm::Value *bx = rop.llvm_load_value (B, 1, i, type);
            llvm::Value *abx = rop.builder().CreateFMul(a, bx);
            llvm::Value *axb = rop.builder().CreateFMul(ax, b);
            llvm::Value *rx = rop.builder().CreateFAdd(abx, axb);
            llvm::Value *ay = rop.llvm_load_value (A, 2, i, type);
            llvm::Value *by = rop.llvm_load_value (B, 2, i, type);
            llvm::Value *aby = rop.builder().CreateFMul(a, by);
            llvm::Value *ayb = rop.builder().CreateFMul(ay, b);
            llvm::Value *ry = rop.builder().CreateFAdd(aby, ayb);
            rop.llvm_store_value (rx, Result, 1, i);
            rop.llvm_store_value (ry, Result, 2, i);
        }
    }

    if (Result.has_derivs() &&  ! (A.has_derivs() || B.has_derivs())) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs (Result);
    }
        
    return true;
}



LLVMGEN (llvm_gen_div)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    ASSERT (! Result.typespec().is_closure_based());

    // division involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_div_m_ff", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_div_fm", Result, A, B);
            else ASSERT (0);
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_div_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_div_mm", Result, A, B);
            else ASSERT (0);
        } else ASSERT (0);
        if (Result.has_derivs())
            rop.llvm_zero_derivs (Result);
        return true;
    }

    // The following should handle f/f, v/v, v/f, f/v, i/i
    // That's all that should be allowed by oslc.
    bool deriv = (Result.has_derivs() && (A.has_derivs() || B.has_derivs()));
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.llvm_load_value (A, 0, i, type);
        llvm::Value *b = rop.llvm_load_value (B, 0, i, type);
        if (!a || !b)
            return false;
        llvm::Value *a_div_b = rop.llvm_make_safe_div (type, a, b);
        llvm::Value *rx = NULL, *ry = NULL;

        if (deriv) {
            // Division of duals: (a/b, 1/b*(ax-a/b*bx), 1/b*(ay-a/b*by))
            ASSERT (is_float);
            llvm::Value *binv = rop.llvm_make_safe_div (type, rop.llvm_constant(1.0f), b);
            llvm::Value *ax = rop.llvm_load_value (A, 1, i, type);
            llvm::Value *bx = rop.llvm_load_value (B, 1, i, type);
            llvm::Value *a_div_b_mul_bx = rop.builder().CreateFMul (a_div_b, bx);
            llvm::Value *ax_minus_a_div_b_mul_bx = rop.builder().CreateFSub (ax, a_div_b_mul_bx);
            rx = rop.builder().CreateFMul (binv, ax_minus_a_div_b_mul_bx);
            llvm::Value *ay = rop.llvm_load_value (A, 2, i, type);
            llvm::Value *by = rop.llvm_load_value (B, 2, i, type);
            llvm::Value *a_div_b_mul_by = rop.builder().CreateFMul (a_div_b, by);
            llvm::Value *ay_minus_a_div_b_mul_by = rop.builder().CreateFSub (ay, a_div_b_mul_by);
            ry = rop.builder().CreateFMul (binv, ay_minus_a_div_b_mul_by);
        }

        rop.llvm_store_value (a_div_b, Result, 0, i);
        if (deriv) {
            rop.llvm_store_value (rx, Result, 1, i);
            rop.llvm_store_value (ry, Result, 2, i);
        }
    }

    if (Result.has_derivs() &&  ! (A.has_derivs() || B.has_derivs())) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs (Result);
    }

    return true;
}



LLVMGEN (llvm_gen_mod)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // The following should handle f%f, v%v, v%f, i%i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = rop.llvm_make_safe_mod (type, a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs()) {
            // Modulus of duals: (a mod b, ax, ay)
            for (int d = 1;  d <= 2;  ++d) {
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *deriv = rop.loadLLVMValue (A, i, d, type);
                    rop.storeLLVMValue (deriv, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_neg)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int d = 0;  d < 3;  ++d) {  // dx, dy
        for (int i = 0; i < num_components; i++) {
            llvm::Value *a = rop.llvm_load_value (A, d, i, type);
            llvm::Value *r = is_float ? rop.builder().CreateFNeg(a)
                                      : rop.builder().CreateNeg(a);
            rop.llvm_store_value (r, Result, d, i);
        }
        if (! Result.has_derivs())
            break;
    }
    return true;
}



// Implementation for clamp
LLVMGEN (llvm_gen_clamp)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& X = *rop.opargsym (op, 1);
    Symbol& Min = *rop.opargsym (op, 2);
    Symbol& Max = *rop.opargsym (op, 3);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        // First do the lower bound
        llvm::Value *val = rop.llvm_load_value (X, 0, i, type);
        llvm::Value *min = rop.llvm_load_value (Min, 0, i, type);
        llvm::Value *cond = is_float ? rop.builder().CreateFCmpULT(val, min)
                                     : rop.builder().CreateICmpSLT(val, min);
        val = rop.builder().CreateSelect (cond, min, val);
        llvm::Value *valdx=NULL, *valdy=NULL;
        if (Result.has_derivs()) {
            valdx = rop.llvm_load_value (X, 1, i, type);
            valdy = rop.llvm_load_value (X, 2, i, type);
            llvm::Value *mindx = rop.llvm_load_value (Min, 1, i, type);
            llvm::Value *mindy = rop.llvm_load_value (Min, 2, i, type);
            valdx = rop.builder().CreateSelect (cond, mindx, valdx);
            valdy = rop.builder().CreateSelect (cond, mindy, valdy);
        }
        // Now do the upper bound
        llvm::Value *max = rop.llvm_load_value (Max, 0, i, type);
        cond = is_float ? rop.builder().CreateFCmpUGT(val, max)
                        : rop.builder().CreateICmpSGT(val, max);
        val = rop.builder().CreateSelect (cond, max, val);
        if (Result.has_derivs()) {
            llvm::Value *maxdx = rop.llvm_load_value (Max, 1, i, type);
            llvm::Value *maxdy = rop.llvm_load_value (Max, 2, i, type);
            valdx = rop.builder().CreateSelect (cond, maxdx, valdx);
            valdy = rop.builder().CreateSelect (cond, maxdy, valdy);
        }
        rop.llvm_store_value (val, Result, 0, i);
        rop.llvm_store_value (valdx, Result, 1, i);
        rop.llvm_store_value (valdy, Result, 2, i);
    }
    return true;
}



// Implementation for min/max
LLVMGEN (llvm_gen_minmax)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& x = *rop.opargsym (op, 1);
    Symbol& y = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        // First do the lower bound
        llvm::Value *x_val = rop.llvm_load_value (x, 0, i, type);
        llvm::Value *y_val = rop.llvm_load_value (y, 0, i, type);

        llvm::Value* cond = NULL;
        // NOTE(boulos): Using <= instead of < to match old behavior
        // (only matters for derivs)
        if (op.opname() == op_min) {
          cond = (is_float) ? rop.builder().CreateFCmpULE(x_val, y_val) :
            rop.builder().CreateICmpSLE(x_val, y_val);
        } else {
          cond = (is_float) ? rop.builder().CreateFCmpUGT(x_val, y_val) :
            rop.builder().CreateICmpSGT(x_val, y_val);
        }

        llvm::Value* res_val = rop.builder().CreateSelect (cond, x_val, y_val);
        rop.llvm_store_value (res_val, Result, 0, i);
        if (Result.has_derivs()) {
          llvm::Value* x_dx = rop.llvm_load_value (x, 1, i, type);
          llvm::Value* x_dy = rop.llvm_load_value (x, 2, i, type);
          llvm::Value* y_dx = rop.llvm_load_value (y, 1, i, type);
          llvm::Value* y_dy = rop.llvm_load_value (y, 2, i, type);
          rop.llvm_store_value (rop.builder().CreateSelect(cond, x_dx, y_dx), Result, 1, i);
          rop.llvm_store_value (rop.builder().CreateSelect(cond, x_dy, y_dy), Result, 2, i);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_bitwise_binary_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);
    ASSERT (Result.typespec().is_int() && A.typespec().is_int() && 
            B.typespec().is_int());

    llvm::Value *a = rop.loadLLVMValue (A);
    llvm::Value *b = rop.loadLLVMValue (B);
    if (!a || !b)
        return false;
    llvm::Value *r = NULL;
    if (op.opname() == op_bitand)
        r = rop.builder().CreateAnd (a, b);
    else if (op.opname() == op_bitor)
        r = rop.builder().CreateOr (a, b);
    else if (op.opname() == op_xor)
        r = rop.builder().CreateXor (a, b);
    else if (op.opname() == op_shl)
        r = rop.builder().CreateShl (a, b);
    else if (op.opname() == op_shr)
        r = rop.builder().CreateAShr (a, b);  // signed int -> arithmetic shift
    else
        return false;
    rop.storeLLVMValue (r, Result);
    return true;
}



// Simple (pointwise) unary ops (Abs, ..., 
LLVMGEN (llvm_gen_unary_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);
        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = 0;
        ustring opname = op.opname();

        if (opname == op_compl) {
            ASSERT (dst.typespec().is_int());
            result = rop.builder().CreateNot(src_val);
        } else {
            // Don't know how to handle this.
            rop.shadingsys().error ("Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        // Store the result
        if (result) {
            // if our op type doesn't match result, convert
            if (dst_float && !src_float) {
                // Op was int, but we need to store float
                result = rop.llvm_int_to_float (result);
            } else if (!dst_float && src_float) {
                // Op was float, but we need to store int
                result = rop.llvm_float_to_int (result);
            } // otherwise just fine
            rop.storeLLVMValue (result, dst, i, 0);
        }

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME!!
        }
    }
    return true;
}



// Simple assignment
LLVMGEN (llvm_gen_assign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    return rop.llvm_assign_impl (Result, Src);
}



// Entire array copying
LLVMGEN (llvm_gen_arraycopy)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    return rop.llvm_assign_impl (Result, Src);
}



// Vector component reference
LLVMGEN (llvm_gen_compref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Val = *rop.opargsym (op, 1);
    Symbol& Index = *rop.opargsym (op, 2);

    llvm::Value *c = rop.llvm_load_value(Index);
    if (rop.shadingsys().range_checking()) {
        if (! (Index.is_constant() &&  *(int *)Index.data() >= 0 &&
               *(int *)Index.data() < 3)) {
            llvm::Value *args[5] = { c, rop.llvm_constant(3),
                                     rop.sg_void_ptr(),
                                     rop.llvm_constant(op.sourcefile()),
                                     rop.llvm_constant(op.sourceline()) };
            c = rop.llvm_call_function ("osl_range_check", args, 5);
            ASSERT (c);
        }
    }

    for (int d = 0;  d < 3;  ++d) {  // deriv
        llvm::Value *val = NULL;
        if (Index.is_constant()) {
            int i = *(int*)Index.data();
            i = Imath::clamp (i, 0, 2);
            val = rop.llvm_load_value (Val, d, i);
        } else {
            val = rop.llvm_load_component_value (Val, d, c);
        }
        rop.llvm_store_value (val, Result, d);
        if (! Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Vector component assignment
LLVMGEN (llvm_gen_compassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Index = *rop.opargsym (op, 1);
    Symbol& Val = *rop.opargsym (op, 2);

    llvm::Value *c = rop.llvm_load_value(Index);
    if (rop.shadingsys().range_checking()) {
        if (! (Index.is_constant() &&  *(int *)Index.data() >= 0 &&
               *(int *)Index.data() < 3)) {
            llvm::Value *args[5] = { c, rop.llvm_constant(3),
                                     rop.sg_void_ptr(),
                                     rop.llvm_constant(op.sourcefile()),
                                     rop.llvm_constant(op.sourceline()) };
            c = rop.llvm_call_function ("osl_range_check", args, 5);
        }
    }

    for (int d = 0;  d < 3;  ++d) {  // deriv
        llvm::Value *val = rop.llvm_load_value (Val, d, 0, TypeDesc::TypeFloat);
        if (Index.is_constant()) {
            int i = *(int*)Index.data();
            i = Imath::clamp (i, 0, 2);
            rop.llvm_store_value (val, Result, d, i);
        } else {
            rop.llvm_store_component_value (val, Result, d, c);
        }
        if (! Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Matrix component reference
LLVMGEN (llvm_gen_mxcompref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& M = *rop.opargsym (op, 1);
    Symbol& Row = *rop.opargsym (op, 2);
    Symbol& Col = *rop.opargsym (op, 3);

    llvm::Value *row = rop.llvm_load_value (Row);
    llvm::Value *col = rop.llvm_load_value (Col);
    if (rop.shadingsys().range_checking()) {
        llvm::Value *args[5] = { row, rop.llvm_constant(4),
                                 rop.sg_void_ptr(),
                                 rop.llvm_constant(op.sourcefile()),
                                 rop.llvm_constant(op.sourceline()) };
        row = rop.llvm_call_function ("osl_range_check", args, 5);
        args[0] = col;
        col = rop.llvm_call_function ("osl_range_check", args, 5);
    }

    llvm::Value *val = NULL; 
    if (Row.is_constant() && Col.is_constant()) {
        int r = Imath::clamp (((int*)Row.data())[0], 0, 3);
        int c = Imath::clamp (((int*)Col.data())[0], 0, 3);
        int comp = 4 * r + c;
        val = rop.llvm_load_value (M, 0, comp);
    } else {
        llvm::Value *comp = rop.builder().CreateMul (row, rop.llvm_constant(4));
        comp = rop.builder().CreateAdd (comp, col);
        val = rop.llvm_load_component_value (M, 0, comp);
    }
    rop.llvm_store_value (val, Result);
    rop.llvm_zero_derivs (Result);

    return true;
}



// Matrix component assignment
LLVMGEN (llvm_gen_mxcompassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Row = *rop.opargsym (op, 1);
    Symbol& Col = *rop.opargsym (op, 2);
    Symbol& Val = *rop.opargsym (op, 3);

    llvm::Value *row = rop.llvm_load_value (Row);
    llvm::Value *col = rop.llvm_load_value (Col);
    if (rop.shadingsys().range_checking()) {
        llvm::Value *args[5] = { row, rop.llvm_constant(4),
                                 rop.sg_void_ptr(),
                                 rop.llvm_constant(op.sourcefile()),
                                 rop.llvm_constant(op.sourceline()) };
        row = rop.llvm_call_function ("osl_range_check", args, 5);
        args[0] = col;
        col = rop.llvm_call_function ("osl_range_check", args, 5);
    }

    llvm::Value *val = rop.llvm_load_value (Val, 0, 0, TypeDesc::TypeFloat);

    if (Row.is_constant() && Col.is_constant()) {
        int r = Imath::clamp (((int*)Row.data())[0], 0, 3);
        int c = Imath::clamp (((int*)Col.data())[0], 0, 3);
        int comp = 4 * r + c;
        rop.llvm_store_value (val, Result, 0, comp);
    } else {
        llvm::Value *comp = rop.builder().CreateMul (row, rop.llvm_constant(4));
        comp = rop.builder().CreateAdd (comp, col);
        rop.llvm_store_component_value (val, Result, 0, comp);
    }
    return true;
}



// Array length
LLVMGEN (llvm_gen_arraylength)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    DASSERT (Result.typespec().is_int() && A.typespec().is_array());

    int len = A.typespec().arraylength();
    rop.llvm_store_value (rop.llvm_constant(len), Result);
    return true;
}



// Array reference
LLVMGEN (llvm_gen_aref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Src = *rop.opargsym (op, 1);
    Symbol& Index = *rop.opargsym (op, 2);

    // Get array index we're interested in
    llvm::Value *index = rop.loadLLVMValue (Index);
    if (! index)
        return false;
    if (rop.shadingsys().range_checking()) {
        if (! (Index.is_constant() &&  *(int *)Index.data() >= 0 &&
               *(int *)Index.data() < Src.typespec().arraylength())) {
            llvm::Value *args[5] = { index,
                                     rop.llvm_constant(Src.typespec().arraylength()),
                                     rop.sg_void_ptr(),
                                     rop.llvm_constant(op.sourcefile()),
                                     rop.llvm_constant(op.sourceline()) };
            index = rop.llvm_call_function ("osl_range_check", args, 5);
        }
    }

    int num_components = Src.typespec().simpletype().aggregate;
    for (int d = 0;  d <= 2;  ++d) {
        for (int c = 0;  c < num_components;  ++c) {
            llvm::Value *val = rop.llvm_load_value (Src, d, index, c);
            rop.storeLLVMValue (val, Result, c, d);
        }
        if (! Result.has_derivs())
            break;
    }

    return true;
}



// Array assignment
LLVMGEN (llvm_gen_aassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Index = *rop.opargsym (op, 1);
    Symbol& Src = *rop.opargsym (op, 2);

    // Get array index we're interested in
    llvm::Value *index = rop.loadLLVMValue (Index);
    if (! index)
        return false;
    if (rop.shadingsys().range_checking()) {
        if (! (Index.is_constant() &&  *(int *)Index.data() >= 0 &&
               *(int *)Index.data() < Result.typespec().arraylength())) {
            llvm::Value *args[5] = { index,
                                     rop.llvm_constant(Result.typespec().arraylength()),
                                     rop.sg_void_ptr(),
                                     rop.llvm_constant(op.sourcefile()),
                                     rop.llvm_constant(op.sourceline()) };
            index = rop.llvm_call_function ("osl_range_check", args, 5);
        }
    }

    int num_components = Result.typespec().simpletype().aggregate;
    for (int d = 0;  d <= 2;  ++d) {
        for (int c = 0;  c < num_components;  ++c) {
            llvm::Value *val = rop.loadLLVMValue (Src, c, d);
            rop.llvm_store_value (val, Result, d, index, c);
        }
        if (! Result.has_derivs())
            break;
    }

    return true;
}



// Construct color, optionally with a color transformation from a named
// color space.
LLVMGEN (llvm_gen_construct_color)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    bool using_space = (op.nargs() == 5);
    Symbol& Space = *rop.opargsym (op, 1);
    Symbol& X = *rop.opargsym (op, 1+using_space);
    Symbol& Y = *rop.opargsym (op, 2+using_space);
    Symbol& Z = *rop.opargsym (op, 3+using_space);
    ASSERT (Result.typespec().is_triple() && X.typespec().is_float() &&
            Y.typespec().is_float() && Z.typespec().is_float() &&
            (using_space == false || Space.typespec().is_string()));

    // First, copy the floats into the vector
    int dmax = Result.has_derivs() ? 3 : 1;
    for (int d = 0;  d < dmax;  ++d) {  // loop over derivs
        for (int c = 0;  c < 3;  ++c) {  // loop over components
            const Symbol& comp = *rop.opargsym (op, c+1+using_space);
            llvm::Value* val = rop.llvm_load_value (comp, d, NULL, 0, TypeDesc::TypeFloat);
            rop.llvm_store_value (val, Result, d, NULL, c);
        }
    }

    // Do the color space conversion in-place, if called for
    if (using_space) {
        llvm::Value *args[3];
        args[0] = rop.sg_void_ptr ();  // shader globals
        args[1] = rop.llvm_void_ptr (Result, 0);  // color
        args[2] = rop.llvm_load_value (Space); // from
        rop.llvm_call_function ("osl_prepend_color_from", args, 3);
        // FIXME(deriv): Punt on derivs for color ctrs with space names.
        // We should try to do this right, but we never had it right for
        // the interpreter, to it's probably not an emergency.
        if (Result.has_derivs())
            rop.llvm_zero_derivs (Result);
    }

    return true;
}



// Construct spatial triple (point, vector, normal), optionally with a
// transformation from a named coordinate system.
LLVMGEN (llvm_gen_construct_triple)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    bool using_space = (op.nargs() == 5);
    Symbol& Space = *rop.opargsym (op, 1);
    Symbol& X = *rop.opargsym (op, 1+using_space);
    Symbol& Y = *rop.opargsym (op, 2+using_space);
    Symbol& Z = *rop.opargsym (op, 3+using_space);
    ASSERT (Result.typespec().is_triple() && X.typespec().is_float() &&
            Y.typespec().is_float() && Z.typespec().is_float() &&
            (using_space == false || Space.typespec().is_string()));

    // First, copy the floats into the vector
    int dmax = Result.has_derivs() ? 3 : 1;
    for (int d = 0;  d < dmax;  ++d) {  // loop over derivs
        for (int c = 0;  c < 3;  ++c) {  // loop over components
            const Symbol& comp = *rop.opargsym (op, c+1+using_space);
            llvm::Value* val = rop.llvm_load_value (comp, d, NULL, 0, TypeDesc::TypeFloat);
            rop.llvm_store_value (val, Result, d, NULL, c);
        }
    }

    // Do the transformation in-place, if called for
    if (using_space) {
        ustring from, to;  // N.B. initialize to empty strings
        if (Space.is_constant()) {
            from = *(ustring *)Space.data();
            if (from == Strings::common ||
                from == rop.shadingsys().commonspace_synonym())
                return true;  // no transformation necessary
        }
        TypeDesc::VECSEMANTICS vectype = TypeDesc::POINT;
        if (op.opname() == "vector")
            vectype = TypeDesc::VECTOR;
        else if (op.opname() == "normal")
            vectype = TypeDesc::NORMAL;
        llvm::Value *args[8] = { rop.sg_void_ptr(),
            rop.llvm_void_ptr(Result), rop.llvm_constant(Result.has_derivs()),
            rop.llvm_void_ptr(Result), rop.llvm_constant(Result.has_derivs()),
            rop.llvm_load_value(Space), rop.llvm_constant(Strings::common),
            rop.llvm_constant((int)vectype) };
        RendererServices *rend (rop.shadingsys().renderer());
        if (rend->transform_points (NULL, from, to, 0.0f, NULL, NULL, 0, vectype)) {
            // renderer potentially knows about a nonlinear transformation.
            // Note that for the case of non-constant strings, passing empty
            // from & to will make transform_points just tell us if ANY 
            // nonlinear transformations potentially are supported.
            rop.llvm_call_function ("osl_transform_triple_nonlinear", args, 8);
        } else {
            // definitely not a nonlinear transformation
            rop.llvm_call_function ("osl_transform_triple", args, 8);
        }
    }

    return true;
}



/// matrix constructor.  Comes in several varieties:
///    matrix (float)
///    matrix (space, float)
///    matrix (...16 floats...)
///    matrix (space, ...16 floats...)
///    matrix (fromspace, tospace)
LLVMGEN (llvm_gen_matrix)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    int nargs = op.nargs();
    bool using_space = (nargs == 3 || nargs == 18);
    bool using_two_spaces = (nargs == 3 && rop.opargsym(op,2)->typespec().is_string());
    int nfloats = nargs - 1 - (int)using_space;
    ASSERT (nargs == 2 || nargs == 3 || nargs == 17 || nargs == 18);

    if (using_two_spaces) {
        llvm::Value *args[4];
        args[0] = rop.sg_void_ptr();  // shader globals
        args[1] = rop.llvm_void_ptr(Result);  // result
        args[2] = rop.llvm_load_value(*rop.opargsym (op, 1));  // from
        args[3] = rop.llvm_load_value(*rop.opargsym (op, 2));  // to
        rop.llvm_call_function ("osl_get_from_to_matrix", args, 4);
    } else {
        if (nfloats == 1) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val = ((i%4) == (i/4)) 
                    ? rop.llvm_load_value (*rop.opargsym(op,1+using_space))
                    : rop.llvm_constant(0.0f);
                rop.llvm_store_value (src_val, Result, 0, i);
            }
        } else if (nfloats == 16) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val = rop.llvm_load_value (*rop.opargsym(op,i+1+using_space));
                rop.llvm_store_value (src_val, Result, 0, i);
            }
        } else {
            ASSERT (0);
        }
        if (using_space) {
            llvm::Value *args[3];
            args[0] = rop.sg_void_ptr();  // shader globals
            args[1] = rop.llvm_void_ptr(Result);  // result
            args[2] = rop.llvm_load_value(*rop.opargsym (op, 1));  // from
            rop.llvm_call_function ("osl_prepend_matrix_from", args, 3);
        }
    }
    if (Result.has_derivs())
        rop.llvm_zero_derivs (Result);
    return true;
}



/// int getmatrix (fromspace, tospace, M)
LLVMGEN (llvm_gen_getmatrix)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    ASSERT (nargs == 4);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& From = *rop.opargsym (op, 1);
    Symbol& To = *rop.opargsym (op, 2);
    Symbol& M = *rop.opargsym (op, 3);

    llvm::Value *args[4];
    args[0] = rop.sg_void_ptr();  // shader globals
    args[1] = rop.llvm_void_ptr(M);  // matrix result
    args[2] = rop.llvm_load_value(From);
    args[3] = rop.llvm_load_value(To);
    llvm::Value *result = rop.llvm_call_function ("osl_get_from_to_matrix", args, 4);
    rop.llvm_store_value (result, Result);
    rop.llvm_zero_derivs (M);
    return true;
}



// transform{,v,n} (string tospace, triple p)
// transform{,v,n} (string fromspace, string tospace, triple p)
// transform{,v,n} (matrix, triple p)
LLVMGEN (llvm_gen_transform)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    Symbol *Result = rop.opargsym (op, 0);
    Symbol *From = (nargs == 3) ? NULL : rop.opargsym (op, 1);
    Symbol *To = rop.opargsym (op, (nargs == 3) ? 1 : 2);
    Symbol *P = rop.opargsym (op, (nargs == 3) ? 2 : 3);

    if (To->typespec().is_matrix()) {
        // llvm_ops has the matrix version already implemented
        llvm_gen_generic (rop, opnum);
        return true;
    }

    // Named space versions from here on out.
    ustring from, to;  // N.B.: initialize to empty strings
    if ((From == NULL || From->is_constant()) && To->is_constant()) {
        // We can know all the space names at this time
        from = From ? *((ustring *)From->data()) : Strings::common;
        to = *((ustring *)To->data());
        ustring syn = rop.shadingsys().commonspace_synonym();
        if (from == syn)
            from = Strings::common;
        if (to == syn)
            to = Strings::common;
        if (from == to) {
            // An identity transformation, just copy
            if (Result != P) // don't bother in-place copy
                rop.llvm_assign_impl (*Result, *P);
            return true;
        }
    }
    TypeDesc::VECSEMANTICS vectype = TypeDesc::POINT;
    if (op.opname() == "transformv")
        vectype = TypeDesc::VECTOR;
    else if (op.opname() == "transformn")
        vectype = TypeDesc::NORMAL;
    llvm::Value *args[8] = { rop.sg_void_ptr(),
        rop.llvm_void_ptr(*P), rop.llvm_constant(P->has_derivs()),
        rop.llvm_void_ptr(*Result), rop.llvm_constant(Result->has_derivs()),
        rop.llvm_load_value(*From), rop.llvm_load_value(*To),
        rop.llvm_constant((int)vectype) };
    RendererServices *rend (rop.shadingsys().renderer());
    if (rend->transform_points (NULL, from, to, 0.0f, NULL, NULL, 0, vectype)) {
        // renderer potentially knows about a nonlinear transformation.
        // Note that for the case of non-constant strings, passing empty
        // from & to will make transform_points just tell us if ANY 
        // nonlinear transformations potentially are supported.
        rop.llvm_call_function ("osl_transform_triple_nonlinear", args, 8);
    } else {
        // definitely not a nonlinear transformation
        rop.llvm_call_function ("osl_transform_triple", args, 8);
    }
    return true;
}



// Derivs
LLVMGEN (llvm_gen_DxDy)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));
    int deriv = (op.opname() == "Dx") ? 1 : 2;

    for (int i = 0; i < Result.typespec().aggregate(); ++i) {
        llvm::Value* src_val = rop.llvm_load_value (Src, deriv, i);
        rop.storeLLVMValue (src_val, Result, i, 0);
    }

    // Don't have 2nd order derivs
    rop.llvm_zero_derivs (Result);
    return true;
}



// Dz
LLVMGEN (llvm_gen_Dz)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    if (&Src == rop.inst()->symbol(rop.inst()->Psym())) {
        // dPdz -- the only Dz we know how to take
        int deriv = 3;
        for (int i = 0; i < Result.typespec().aggregate(); ++i) {
            llvm::Value* src_val = rop.llvm_load_value (Src, deriv, i);
            rop.storeLLVMValue (src_val, Result, i, 0);
        }
        // Don't have 2nd order derivs
        rop.llvm_zero_derivs (Result);
    } else {
        // Punt, everything else for now returns 0 for Dz
        // FIXME?
        rop.llvm_assign_zero (Result);
    }
    return true;
}



LLVMGEN (llvm_gen_filterwidth)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    ASSERT (Src.typespec().is_float() || Src.typespec().is_triple());
    if (Src.has_derivs()) {
        if (Src.typespec().is_float()) {
            llvm::Value *r = rop.llvm_call_function ("osl_filterwidth_fdf",
                                                     rop.llvm_void_ptr (Src));
            rop.llvm_store_value (r, Result);
        } else {
            rop.llvm_call_function ("osl_filterwidth_vdv",
                                    rop.llvm_void_ptr (Result),
                                    rop.llvm_void_ptr (Src));
        }
        // Don't have 2nd order derivs
        rop.llvm_zero_derivs (Result);
    } else {
        // No derivs to be had
        rop.llvm_assign_zero (Src);
    }

    return true;
}



// Comparison ops
LLVMGEN (llvm_gen_compare_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &A (*rop.opargsym (op, 1));
    Symbol &B (*rop.opargsym (op, 2));
    ASSERT (Result.typespec().is_int() && ! Result.has_derivs());

    int num_components = std::max (A.typespec().aggregate(), B.typespec().aggregate());
    bool float_based = A.typespec().is_floatbased() || B.typespec().is_floatbased();
    TypeDesc cast (float_based ? TypeDesc::FLOAT : TypeDesc::UNKNOWN);

    llvm::Value* final_result = 0;
    ustring opname = op.opname();

    for (int i = 0; i < num_components; i++) {
        // Get A&B component i -- note that these correctly handle mixed
        // scalar/triple comparisons as well as int->float casts as needed.
        llvm::Value* a = rop.loadLLVMValue (A, i, 0, cast);
        llvm::Value* b = rop.loadLLVMValue (B, i, 0, cast);

        // Trickery for mixed matrix/scalar comparisons -- compare
        // on-diagonal to the scalar, off-diagonal to zero
        if (A.typespec().is_matrix() && !B.typespec().is_matrix()) {
            if ((i/4) != (i%4))
                b = rop.llvm_constant (0.0f);
        }
        if (! A.typespec().is_matrix() && B.typespec().is_matrix()) {
            if ((i/4) != (i%4))
                a = rop.llvm_constant (0.0f);
        }

        // Perform the op
        llvm::Value* result = 0;
        if (opname == op_lt) {
            result = float_based ? rop.builder().CreateFCmpULT(a, b) : rop.builder().CreateICmpSLT(a, b);
        } else if (opname == op_le) {
            result = float_based ? rop.builder().CreateFCmpULE(a, b) : rop.builder().CreateICmpSLE(a, b);
        } else if (opname == op_eq) {
            result = float_based ? rop.builder().CreateFCmpUEQ(a, b) : rop.builder().CreateICmpEQ(a, b);
        } else if (opname == op_ge) {
            result = float_based ? rop.builder().CreateFCmpUGE(a, b) : rop.builder().CreateICmpSGE(a, b);
        } else if (opname == op_gt) {
            result = float_based ? rop.builder().CreateFCmpUGT(a, b) : rop.builder().CreateICmpSGT(a, b);
        } else if (opname == op_neq) {
            result = float_based ? rop.builder().CreateFCmpUNE(a, b) : rop.builder().CreateICmpNE(a, b);
        } else {
            // Don't know how to handle this.
            ASSERT (0 && "Comparison error");
        }
        ASSERT (result);

        if (final_result) {
            // Combine the component bool based on the op
            if (opname != op_neq)        // final_result &= result
                final_result = rop.builder().CreateAnd(final_result, result);
            else                         // final_result |= result
                final_result = rop.builder().CreateOr(final_result, result);
        } else {
            final_result = result;
        }
    }
    ASSERT (final_result);

    // Convert the single bit bool into an int for now.
    final_result = rop.builder().CreateZExt (final_result, rop.llvm_type_int());
    rop.storeLLVMValue (final_result, Result, 0, 0);
    return true;
}



// int regex_search (string subject, string pattern)
// int regex_search (string subject, int results[], string pattern)
// int regex_match (string subject, string pattern)
// int regex_match (string subject, int results[], string pattern)
LLVMGEN (llvm_gen_regex)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    ASSERT (nargs == 3 || nargs == 4);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &Subject (*rop.opargsym (op, 1));
    bool do_match_results = (nargs == 4);
    bool fullmatch = (op.opname() == "regex_match");
    Symbol &Match (*rop.opargsym (op, 2));
    Symbol &Pattern (*rop.opargsym (op, 2+do_match_results));
    ASSERT (Result.typespec().is_int() && Subject.typespec().is_string() &&
            Pattern.typespec().is_string());
    ASSERT (!do_match_results || 
            (Match.typespec().is_array() &&
             Match.typespec().elementtype().is_int()));

    std::vector<llvm::Value*> call_args;
    // First arg is ShaderGlobals ptr
    call_args.push_back (rop.sg_void_ptr());
    // Next arg is subject string
    call_args.push_back (rop.llvm_load_value (Subject));
    // Pass the results array and length (just pass 0 if no results wanted).
    call_args.push_back (rop.llvm_void_ptr(Match));
    if (do_match_results)
        call_args.push_back (rop.llvm_constant(Match.typespec().arraylength()));
    else
        call_args.push_back (rop.llvm_constant(0));
    // Pass the regex match pattern
    call_args.push_back (rop.llvm_load_value (Pattern));
    // Pass whether or not to do the full match
    call_args.push_back (rop.llvm_constant(fullmatch));

    llvm::Value *ret = rop.llvm_call_function ("osl_regex_impl", &call_args[0],
                                               (int)call_args.size());
    rop.llvm_store_value (ret, Result);
    return true;
}



// Generic llvm code generation.  See the comments in llvm_ops.cpp for
// the full list of assumptions and conventions.  But in short:
//   1. All polymorphic and derivative cases implemented as functions in
//      llvm_ops.cpp -- no custom IR is needed.
//   2. Naming conention is: osl_NAME_{args}, where args is the
//      concatenation of type codes for all args including return value --
//      f/i/v/m/s for float/int/triple/matrix/string, and df/dv/dm for
//      duals.
//   3. The function returns scalars as an actual return value (that
//      must be stored), but "returns" aggregates or duals in the first
//      argument.
//   4. Duals and aggregates are passed as void*'s, float/int/string 
//      passed by value.
//   5. Note that this only works if triples are all treated identically,
//      this routine can't be used if it must be polymorphic based on
//      color, point, vector, normal differences.
//
LLVMGEN (llvm_gen_generic)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result  = *rop.opargsym (op, 0);
    std::vector<const Symbol *> args;
    bool any_deriv_args = false;
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        args.push_back (s);
        any_deriv_args |= (i > 0 && s->has_derivs() && !s->typespec().is_matrix());
    }

    // Special cases: functions that have no derivs -- suppress them
    if (any_deriv_args)
        if (op.opname() == op_floor || op.opname() == op_ceil ||
            op.opname() == op_round || op.opname() == op_step ||
            op.opname() == op_trunc || op.opname() == op_cellnoise ||
            op.opname() == op_sign)
            any_deriv_args = false;

    std::string name = std::string("osl_") + op.opname().string() + "_";
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        if (any_deriv_args && Result.has_derivs() && s->has_derivs() && !s->typespec().is_matrix())
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else if (s->typespec().is_matrix())
            name += "m";
        else if (s->typespec().is_string())
            name += "s";
        else if (s->typespec().is_int())
            name += "i";
        else ASSERT (0);
    }

    if (! Result.has_derivs() || ! any_deriv_args) {
        // Don't compute derivs -- either not needed or not provided in args
        if (Result.typespec().aggregate() == TypeDesc::SCALAR) {
            llvm::Value *r = rop.llvm_call_function (name.c_str(),
                                                     &(args[1]), op.nargs()-1);
            rop.llvm_store_value (r, Result);
        } else {
            rop.llvm_call_function (name.c_str(), &(args[0]), op.nargs());
        }
        rop.llvm_zero_derivs (Result);
    } else {
        // Cases with derivs
        ASSERT (Result.has_derivs() && any_deriv_args);
        rop.llvm_call_function (name.c_str(), &(args[0]), op.nargs(), true);
    }
    return true;
}



LLVMGEN (llvm_gen_sincos)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Theta   = *rop.opargsym (op, 0);
    Symbol& Sin_out = *rop.opargsym (op, 1);
    Symbol& Cos_out = *rop.opargsym (op, 2);
    std::vector<llvm::Value *> valargs;
    bool theta_deriv   = Theta.has_derivs();
    bool result_derivs = (Sin_out.has_derivs() || Cos_out.has_derivs());

    std::string name = std::string("osl_sincos_");
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        if (s->has_derivs() && result_derivs  && theta_deriv)
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else ASSERT (0);
    }
    // push back llvm arguments
    valargs.push_back ( (theta_deriv && result_derivs) || Theta.typespec().is_triple() ? 
          rop.llvm_void_ptr (Theta) : rop.llvm_load_value (Theta));
    valargs.push_back (rop.llvm_void_ptr (Sin_out));
    valargs.push_back (rop.llvm_void_ptr (Cos_out));

    rop.llvm_call_function (name.c_str(), &valargs[0], 3);

    return true;
}

LLVMGEN (llvm_gen_andor)
{
    Opcode& op (rop.inst()->ops()[opnum]);
    Symbol& result = *rop.opargsym (op, 0);
    Symbol& a = *rop.opargsym (op, 1);
    Symbol& b = *rop.opargsym (op, 2);

    llvm::Value* i1_res = NULL;
    llvm::Value* a_val = rop.llvm_load_value (a, 0, 0, TypeDesc::TypeInt);
    llvm::Value* b_val = rop.llvm_load_value (b, 0, 0, TypeDesc::TypeInt);
    if (op.opname() == op_and) {
        // From the old bitcode generated
        // define i32 @osl_and_iii(i32 %a, i32 %b) nounwind readnone ssp {
        //     %1 = icmp ne i32 %b, 0
        //  %not. = icmp ne i32 %a, 0
        //     %2 = and i1 %1, %not.
        //     %3 = zext i1 %2 to i32
        //   ret i32 %3
        llvm::Value* b_ne_0 = rop.builder().CreateICmpNE (b_val, rop.llvm_constant(0));
        llvm::Value* a_ne_0 = rop.builder().CreateICmpNE (a_val, rop.llvm_constant(0));
        llvm::Value* both_ne_0 = rop.builder().CreateAnd (b_ne_0, a_ne_0);
        i1_res = both_ne_0;
    } else {
        // Also from the bitcode
        // %1 = or i32 %b, %a
        // %2 = icmp ne i32 %1, 0
        // %3 = zext i1 %2 to i32
        llvm::Value* or_ab = rop.builder().CreateOr(a_val, b_val);
        llvm::Value* or_ab_ne_0 = rop.builder().CreateICmpNE (or_ab, rop.llvm_constant(0));
        i1_res = or_ab_ne_0;
    }
    llvm::Value* i32_res = rop.builder().CreateZExt(i1_res, rop.llvm_type_int());
    rop.llvm_store_value(i32_res, result, 0, 0);
    return true;
}


LLVMGEN (llvm_gen_if)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym (op, 0);

    // Load the condition variable and figure out if it's nonzero
    llvm::Value* cond_val = rop.llvm_load_value (cond, 0, 0, TypeDesc::TypeInt);
    cond_val = rop.builder().CreateICmpNE (cond_val, rop.llvm_constant(0));
    
    // Branch on the condition, to our blocks
    llvm::BasicBlock* then_block = rop.llvm_new_basic_block ("then");
    llvm::BasicBlock* else_block = rop.llvm_new_basic_block ("else");
    llvm::BasicBlock* after_block = rop.llvm_new_basic_block ("");
    rop.builder().CreateCondBr (cond_val, then_block, else_block);

    // Then block
    rop.build_llvm_code (opnum+1, op.jump(0), then_block);
    rop.builder().CreateBr (after_block);

    // Else block
    rop.build_llvm_code (op.jump(0), op.jump(1), else_block);
    rop.builder().CreateBr (after_block);

    // Continue on with the previous flow
    rop.builder().SetInsertPoint (after_block);
    return true;
}



LLVMGEN (llvm_gen_loop_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym (op, 0);

    // Branch on the condition, to our blocks
    llvm::BasicBlock* cond_block = rop.llvm_new_basic_block ("cond");
    llvm::BasicBlock* body_block = rop.llvm_new_basic_block ("body");
    llvm::BasicBlock* step_block = rop.llvm_new_basic_block ("step");
    llvm::BasicBlock* after_block = rop.llvm_new_basic_block ("");
    // Save the step and after block pointers for possible break/continue
    rop.llvm_push_loop (step_block, after_block);

    // Initialization (will be empty except for "for" loops)
    rop.build_llvm_code (opnum+1, op.jump(0));

    // For "do-while", we go straight to the body of the loop, but for
    // "for" or "while", we test the condition next.
    rop.builder().CreateBr (op.opname() == op_dowhile ? body_block : cond_block);

    // Load the condition variable and figure out if it's nonzero
    rop.build_llvm_code (op.jump(0), op.jump(1), cond_block);
    llvm::Value* cond_val = rop.llvm_load_value (cond, 0, 0, TypeDesc::TypeInt);
    cond_val = rop.builder().CreateICmpNE (cond_val, rop.llvm_constant(0));
    // Jump to either LoopBody or AfterLoop
    rop.builder().CreateCondBr (cond_val, body_block, after_block);

    // Body of loop
    rop.build_llvm_code (op.jump(1), op.jump(2), body_block);
    rop.builder().CreateBr (step_block);

    // Step
    rop.build_llvm_code (op.jump(2), op.jump(3), step_block);
    rop.builder().CreateBr (cond_block);

    // Continue on with the previous flow
    rop.builder().SetInsertPoint (after_block);
    rop.llvm_pop_loop ();

    return true;
}



LLVMGEN (llvm_gen_loopmod_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    DASSERT (op.nargs() == 0);
    if (op.opname() == op_break) {
        rop.builder().CreateBr (rop.llvm_loop_after_block());
    } else {  // continue
        rop.builder().CreateBr (rop.llvm_loop_step_block());
    }
    llvm::BasicBlock* next_block = rop.llvm_new_basic_block ("");
    rop.builder().SetInsertPoint (next_block);
    return true;
}



static llvm::Value *
llvm_gen_texture_options (RuntimeOptimizer &rop, int opnum,
                          int first_optional_arg, bool tex3d,
                          llvm::Value* &alpha, llvm::Value* &dalphadx,
                          llvm::Value* &dalphady)
{
    // Reserve space for the TextureOpt, with alignment
    size_t tosize = (sizeof(TextureOpt)+sizeof(char*)-1) / sizeof(char*);
    llvm::Value* opt = rop.builder().CreateAlloca(rop.llvm_type_void_ptr(),
                                                  rop.llvm_constant((int)tosize));
    opt = rop.llvm_void_ptr (opt);
    rop.llvm_call_function ("osl_texture_clear", opt);

    Opcode &op (rop.inst()->ops()[opnum]);
    for (int a = first_optional_arg;  a < op.nargs();  ++a) {
        Symbol &Name (*rop.opargsym(op,a));
        ASSERT (Name.typespec().is_string() &&
                "optional texture token must be a string");
        ASSERT (a+1 < op.nargs() && "malformed argument list for texture");
        ustring name = *(ustring *)Name.data();

        ++a;  // advance to next argument
        Symbol &Val (*rop.opargsym(op,a));
        TypeDesc valtype = Val.typespec().simpletype ();
        
        if (! name)    // skip empty string param name
            continue;
        llvm::Value *val = rop.llvm_load_value (Val);

        // If certain float-expecting options were passed an int, do the
        // conversion automatically.
        if (valtype == TypeDesc::INT &&
            (name == Strings::width || name == Strings::swidth ||
             name == Strings::twidth || name == Strings::rwidth ||
             name == Strings::blur || name == Strings::sblur ||
             name == Strings::tblur || name == Strings::rblur)) {
            val = rop.llvm_int_to_float (val);
            valtype = TypeDesc::FLOAT;
        }

        if (name == Strings::width && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_swidth", opt, val);
            rop.llvm_call_function ("osl_texture_set_twidth", opt, val);
            if (tex3d)
                rop.llvm_call_function ("osl_texture_set_rwidth", opt, val);
        } else if (name == Strings::swidth && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_swidth", opt, val);
        } else if (name == Strings::twidth && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_twidth", opt, val);
        } else if (name == Strings::rwidth && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_rwidth", opt, val);

        } else if (name == Strings::blur && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_sblur", opt, val);
            rop.llvm_call_function ("osl_texture_set_tblur", opt, val);
            if (tex3d)
                rop.llvm_call_function ("osl_texture_set_rblur",opt, val);
        } else if (name == Strings::sblur && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_sblur", opt, val);
        } else if (name == Strings::tblur && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_tblur", opt, val);
        } else if (name == Strings::rblur && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_rblur", opt, val);

        } else if (name == Strings::wrap && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                int mode = TextureOpt::decode_wrapmode (*(char **)Val.data());
                val = rop.llvm_constant (mode);
                rop.llvm_call_function ("osl_texture_set_swrap_code", opt, val);
                rop.llvm_call_function ("osl_texture_set_twrap_code", opt, val);
                if (tex3d)
                    rop.llvm_call_function ("osl_texture_set_rwrap_code", opt, val);
            } else {
                rop.llvm_call_function ("osl_texture_set_swrap", opt, val);
                rop.llvm_call_function ("osl_texture_set_twrap", opt, val);
                if (tex3d)
                    rop.llvm_call_function ("osl_texture_set_rwrap", opt, val);
            }
        } else if (name == Strings::swrap && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                int mode = TextureOpt::decode_wrapmode (*(char **)Val.data());
                val = rop.llvm_constant (mode);
                rop.llvm_call_function ("osl_texture_set_swrap_code", opt, val);
            } else 
                rop.llvm_call_function ("osl_texture_set_swrap", opt, val);
        } else if (name == Strings::twrap && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                int mode = TextureOpt::decode_wrapmode (*(char **)Val.data());
                val = rop.llvm_constant (mode);
                rop.llvm_call_function ("osl_texture_set_twrap_code", opt, val);
            } else
                rop.llvm_call_function ("osl_texture_set_twrap", opt, val);
        } else if (name == Strings::rwrap && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                int mode = TextureOpt::decode_wrapmode (*(char **)Val.data());
                val = rop.llvm_constant (mode);
                rop.llvm_call_function ("osl_texture_set_rwrap_code", opt, val);
            } else
                rop.llvm_call_function ("osl_texture_set_rwrap", opt, val);

        } else if (name == Strings::firstchannel && valtype == TypeDesc::INT) {
            rop.llvm_call_function ("osl_texture_set_firstchannel", opt, val);
        } else if (name == Strings::fill && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_fill", opt, val);
        } else if (name == Strings::time && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_texture_set_time", opt, val);

        } else if (name == Strings::interp && valtype == TypeDesc::STRING) {
            // Try to decode the interp name string into an integer mode,
            // so it doesn't have to happen at runtime.
            int mode = -1;
            if (Val.is_constant())
                mode = tex_interp_to_code (*(ustring *)Val.data());
            if (mode >= 0)
                rop.llvm_call_function ("osl_texture_set_interp_code", opt,
                                        rop.llvm_constant(mode));
            else
                rop.llvm_call_function ("osl_texture_set_interp_name", opt, val);

        } else if (name == Strings::alpha && valtype == TypeDesc::FLOAT) {
            alpha = rop.llvm_get_pointer (Val);
            if (Val.has_derivs()) {
                dalphadx = rop.llvm_get_pointer (Val, 1);
                dalphady = rop.llvm_get_pointer (Val, 2);
                // NO z derivs!  dalphadz = rop.llvm_get_pointer (Val, 3);
            }
        } else {
            rop.shadingsys().error ("Unknown texture%s optional argument: \"%s\", <%s> (%s:%d)",
                                    tex3d ? "3d" : "",
                                    name.c_str(), valtype.c_str(),
                                    op.sourcefile().c_str(), op.sourceline());
        }
    }

    return opt;
}



LLVMGEN (llvm_gen_texture)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result = *rop.opargsym (op, 0);
    Symbol &Filename = *rop.opargsym (op, 1);
    Symbol &S = *rop.opargsym (op, 2);
    Symbol &T = *rop.opargsym (op, 3);

    bool user_derivs = false;
    int first_optional_arg = 4;
    if (op.nargs() > 4 && rop.opargsym(op,4)->typespec().is_float()) {
        user_derivs = true;
        first_optional_arg = 8;
        DASSERT (rop.opargsym(op,5)->typespec().is_float());
        DASSERT (rop.opargsym(op,6)->typespec().is_float());
        DASSERT (rop.opargsym(op,7)->typespec().is_float());
    }

    llvm::Value* opt;   // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    opt = llvm_gen_texture_options (rop, opnum, first_optional_arg,
                                    false /*3d*/, alpha, dalphadx, dalphady);

    // Now call the osl_texture function, passing the options and all the
    // explicit args like texture coordinates.
    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_load_value (Filename));
    args.push_back (opt);
    args.push_back (rop.llvm_load_value (S));
    args.push_back (rop.llvm_load_value (T));
    if (user_derivs) {
        args.push_back (rop.llvm_load_value (*rop.opargsym (op, 4)));
        args.push_back (rop.llvm_load_value (*rop.opargsym (op, 5)));
        args.push_back (rop.llvm_load_value (*rop.opargsym (op, 6)));
        args.push_back (rop.llvm_load_value (*rop.opargsym (op, 7)));
    } else {
        // Auto derivs of S and T
        args.push_back (rop.llvm_load_value (S, 1));
        args.push_back (rop.llvm_load_value (T, 1));
        args.push_back (rop.llvm_load_value (S, 2));
        args.push_back (rop.llvm_load_value (T, 2));
    }
    args.push_back (rop.llvm_constant ((int)Result.typespec().aggregate()));
    args.push_back (rop.llvm_void_ptr (rop.llvm_get_pointer (Result, 0)));
    args.push_back (rop.llvm_void_ptr (rop.llvm_get_pointer (Result, 1)));
    args.push_back (rop.llvm_void_ptr (rop.llvm_get_pointer (Result, 2)));
    if (alpha) {
        args.push_back (rop.llvm_void_ptr (alpha));
        args.push_back (rop.llvm_void_ptr (dalphadx ? dalphadx : rop.llvm_void_ptr_null()));
        args.push_back (rop.llvm_void_ptr (dalphady ? dalphady : rop.llvm_void_ptr_null()));
        rop.llvm_call_function ("osl_texture_alpha", &args[0], (int)args.size());
    } else {
        rop.llvm_call_function ("osl_texture", &args[0], (int)args.size());
    }
    return true;
}



LLVMGEN (llvm_gen_texture3d)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result = *rop.opargsym (op, 0);
    Symbol &Filename = *rop.opargsym (op, 1);
    Symbol &P = *rop.opargsym (op, 2);

    bool user_derivs = false;
    int first_optional_arg = 4;
    if (op.nargs() > 3 && rop.opargsym(op,3)->typespec().is_triple()) {
        user_derivs = true;
        first_optional_arg = 6;
        DASSERT (rop.opargsym(op,4)->typespec().is_triple());
        DASSERT (rop.opargsym(op,5)->typespec().is_triple());
    }

    llvm::Value* opt;   // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    opt = llvm_gen_texture_options (rop, opnum, first_optional_arg,
                                    true /*3d*/, alpha, dalphadx, dalphady);

    // Now call the osl_texture3d function, passing the options and all the
    // explicit args like texture coordinates.
    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_load_value (Filename));
    args.push_back (opt);
    args.push_back (rop.llvm_void_ptr (P));
    if (user_derivs) {
        args.push_back (rop.llvm_void_ptr (*rop.opargsym (op, 3)));
        args.push_back (rop.llvm_void_ptr (*rop.opargsym (op, 4)));
        args.push_back (rop.llvm_void_ptr (*rop.opargsym (op, 5)));
    } else {
        // Auto derivs of P
        args.push_back (rop.llvm_void_ptr (P, 1));
        args.push_back (rop.llvm_void_ptr (P, 2));
        // dPdz is correct for input P, zero for all else
        if (&P == rop.inst()->symbol(rop.inst()->Psym())) {
            args.push_back (rop.llvm_void_ptr (P, 3));
        } else {
            // zero for dPdz, for now
            llvm::Value *fzero = rop.llvm_constant (0.0f);
            llvm::Value *vzero = rop.builder().CreateAlloca (rop.llvm_type_triple(),
                                                     rop.llvm_constant((int)1));
            for (int i = 0;  i < 3;  ++i)
                rop.builder().CreateStore (fzero, rop.builder().CreateConstGEP2_32 (vzero, 0, i));
            args.push_back (rop.llvm_void_ptr(vzero));
        }
    }
    args.push_back (rop.llvm_constant ((int)Result.typespec().aggregate()));
    args.push_back (rop.llvm_void_ptr (rop.llvm_void_ptr (Result, 0)));
    args.push_back (rop.llvm_void_ptr (rop.llvm_void_ptr (Result, 1)));
    args.push_back (rop.llvm_void_ptr (rop.llvm_void_ptr (Result, 2)));
    args.push_back (rop.llvm_void_ptr_null());  // no dresultdz for now
    if (alpha) {
        args.push_back (rop.llvm_void_ptr (alpha));
        args.push_back (dalphadx ? rop.llvm_void_ptr (dalphadx) : rop.llvm_void_ptr_null());
        args.push_back (dalphady ? rop.llvm_void_ptr (dalphady) : rop.llvm_void_ptr_null());
        args.push_back (rop.llvm_void_ptr_null());  // No dalphadz for now
        rop.llvm_call_function ("osl_texture3d_alpha", &args[0], (int)args.size());
    } else {
        rop.llvm_call_function ("osl_texture3d", &args[0], (int)args.size());
    }
    return true;
}



LLVMGEN (llvm_gen_environment)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result = *rop.opargsym (op, 0);
    Symbol &Filename = *rop.opargsym (op, 1);
    Symbol &R = *rop.opargsym (op, 2);

    bool user_derivs = false;
    int first_optional_arg = 3;
    if (op.nargs() > 3 && rop.opargsym(op,3)->typespec().is_triple()) {
        user_derivs = true;
        first_optional_arg = 5;
        DASSERT (rop.opargsym(op,4)->typespec().is_triple());
    }

    llvm::Value* opt;   // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    opt = llvm_gen_texture_options (rop, opnum, first_optional_arg,
                                    false /*3d*/, alpha, dalphadx, dalphady);

    // Now call the osl_environment function, passing the options and all the
    // explicit args like texture coordinates.
    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_load_value (Filename));
    args.push_back (opt);
    args.push_back (rop.llvm_void_ptr (R));
    if (user_derivs) {
        args.push_back (rop.llvm_void_ptr (*rop.opargsym (op, 3)));
        args.push_back (rop.llvm_void_ptr (*rop.opargsym (op, 4)));
    } else {
        // Auto derivs of R
        args.push_back (rop.llvm_void_ptr (R, 1));
        args.push_back (rop.llvm_void_ptr (R, 2));
    }
    args.push_back (rop.llvm_constant ((int)Result.typespec().aggregate()));
    args.push_back (rop.llvm_void_ptr (Result, 0));
    args.push_back (rop.llvm_void_ptr (Result, 1));
    args.push_back (rop.llvm_void_ptr (Result, 2));
    if (alpha) {
        args.push_back (rop.llvm_void_ptr (alpha));
        args.push_back (dalphadx ? rop.llvm_void_ptr (dalphadx) : rop.llvm_void_ptr_null());
        args.push_back (dalphady ? rop.llvm_void_ptr (dalphady) : rop.llvm_void_ptr_null());
    } else {
        args.push_back (rop.llvm_void_ptr_null());
        args.push_back (rop.llvm_void_ptr_null());
        args.push_back (rop.llvm_void_ptr_null());
    }
    rop.llvm_call_function ("osl_environment", &args[0], (int)args.size());
    return true;
}



static llvm::Value *
llvm_gen_trace_options (RuntimeOptimizer &rop, int opnum,
                        int first_optional_arg)
{
    // Reserve space for the TraceOpt, with alignment
    size_t tosize = (sizeof(RendererServices::TraceOpt)+sizeof(char*)-1) / sizeof(char*);
    llvm::Value* opt = rop.builder().CreateAlloca(rop.llvm_type_void_ptr(),
                                                  rop.llvm_constant((int)tosize));
    opt = rop.llvm_void_ptr (opt);
    rop.llvm_call_function ("osl_trace_clear", opt);

    Opcode &op (rop.inst()->ops()[opnum]);
    for (int a = first_optional_arg;  a < op.nargs();  ++a) {
        Symbol &Name (*rop.opargsym(op,a));
        ASSERT (Name.typespec().is_string() &&
                "optional trace token must be a string");
        ASSERT (a+1 < op.nargs() && "malformed argument list for trace");
        ustring name = *(ustring *)Name.data();

        ++a;  // advance to next argument
        Symbol &Val (*rop.opargsym(op,a));
        TypeDesc valtype = Val.typespec().simpletype ();
        
        llvm::Value *val = rop.llvm_load_value (Val);
        static ustring kmindist("mindist"), kmaxdist("maxdist");
        static ustring kshade("shade");
        if (name == kmindist && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_trace_set_mindist", opt, val);
        } else if (name == kmaxdist && valtype == TypeDesc::FLOAT) {
            rop.llvm_call_function ("osl_trace_set_maxdist", opt, val);
        } else if (name == kshade && valtype == TypeDesc::INT) {
            rop.llvm_call_function ("osl_trace_set_shade", opt, val);
        } else {
            rop.shadingsys().error ("Unknown trace() optional argument: \"%s\", <%s> (%s:%d)",
                                    name.c_str(), valtype.c_str(),
                                    op.sourcefile().c_str(), op.sourceline());
        }
    }

    return opt;
}



LLVMGEN (llvm_gen_trace)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result = *rop.opargsym (op, 0);
    Symbol &Pos = *rop.opargsym (op, 1);
    Symbol &Dir = *rop.opargsym (op, 2);
    int first_optional_arg = 3;

    llvm::Value* opt;   // TraceOpt
    opt = llvm_gen_trace_options (rop, opnum, first_optional_arg);

    // Now call the osl_trace function, passing the options and all the
    // explicit args like trace coordinates.
    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());
    args.push_back (opt);
    args.push_back (rop.llvm_void_ptr (Pos, 0));
    args.push_back (rop.llvm_void_ptr (Pos, 1));
    args.push_back (rop.llvm_void_ptr (Pos, 2));
    args.push_back (rop.llvm_void_ptr (Dir, 0));
    args.push_back (rop.llvm_void_ptr (Dir, 1));
    args.push_back (rop.llvm_void_ptr (Dir, 2));
    llvm::Value *r = rop.llvm_call_function ("osl_trace", &args[0],
                                             (int)args.size());
    rop.llvm_store_value (r, Result);
    return true;
}



// pnoise and psnoise -- we can't use llvm_gen_generic because of the
// special case that the periods should never pass derivatives.
LLVMGEN (llvm_gen_pnoise)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    // N.B. we don't use the derivatives of periods.  There are as many
    // period arguments as position arguments, and argument 0 is the
    // result.  So f=pnoise(f,f) => firstperiod = 2; f=pnoise(v,f,v,f)
    // => firstperiod = 3.
    int firstperiod = (op.nargs() - 1) / 2 + 1;

    Symbol& Result  = *rop.opargsym (op, 0);
    bool any_deriv_args = false;
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        any_deriv_args |= (i > 0 && i < firstperiod &&
                           s->has_derivs() && !s->typespec().is_matrix());
    }

    std::string name = std::string("osl_") + op.opname().string() + "_";
    std::vector<llvm::Value *> valargs;
    valargs.resize (op.nargs());
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        bool use_derivs = any_deriv_args && i < firstperiod && Result.has_derivs() && s->has_derivs() && !s->typespec().is_matrix();
        if (use_derivs)
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else ASSERT (0);


        if (s->typespec().simpletype().aggregate > 1 || use_derivs)
            valargs[i] = rop.llvm_void_ptr (*s);
        else
            valargs[i] = rop.llvm_load_value (*s);
    }

    if (! Result.has_derivs() || ! any_deriv_args) {
        // Don't compute derivs -- either not needed or not provided in args
        if (Result.typespec().aggregate() == TypeDesc::SCALAR) {
            llvm::Value *r = rop.llvm_call_function (name.c_str(), &valargs[1], op.nargs()-1);
            rop.llvm_store_value (r, Result);
        } else {
            rop.llvm_call_function (name.c_str(), &valargs[0], op.nargs());
        }
        rop.llvm_zero_derivs (Result);
    } else {
        // Cases with derivs
        ASSERT (Result.has_derivs() && any_deriv_args);
        rop.llvm_call_function (name.c_str(), &valargs[0], op.nargs());
    }
    return true;
}



LLVMGEN (llvm_gen_getattribute)
{
    // getattribute() has eight "flavors":
    //   * getattribute (attribute_name, value)
    //   * getattribute (attribute_name, value[])
    //   * getattribute (attribute_name, index, value)
    //   * getattribute (attribute_name, index, value[])
    //   * getattribute (object, attribute_name, value)
    //   * getattribute (object, attribute_name, value[])
    //   * getattribute (object, attribute_name, index, value)
    //   * getattribute (object, attribute_name, index, value[])
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() >= 3 && op.nargs() <= 5);

    bool object_lookup = false;
    bool array_lookup  = false;

    // slot indices when (nargs==3)
    int result_slot = 0; // never changes
    int attrib_slot = 1;
    int object_slot = 0; // initially not used
    int index_slot  = 0; // initially not used
    int dest_slot   = 2;

    // figure out which "flavor" of getattribute() to use
    if (op.nargs() == 5) {
        object_slot = 1;
        attrib_slot = 2;
        index_slot  = 3;
        dest_slot   = 4;
        array_lookup  = true;
        object_lookup = true;
    }
    else if (op.nargs() == 4) {
        if (rop.opargsym (op, 2)->typespec().is_int()) {
            attrib_slot = 1;
            index_slot  = 2;
            dest_slot   = 3;
            array_lookup = true;
        }
        else {
            object_slot = 1;
            attrib_slot = 2;
            dest_slot   = 3;
            object_lookup = true;
        }
    }

    Symbol& Result      = *rop.opargsym (op, result_slot);
    Symbol& ObjectName  = *rop.opargsym (op, object_slot); // might be aliased to Result
    Symbol& Index       = *rop.opargsym (op, index_slot);  // might be aliased to Result
    Symbol& Attribute   = *rop.opargsym (op, attrib_slot);
    Symbol& Destination = *rop.opargsym (op, dest_slot);

    TypeDesc attribute_type = Destination.typespec().simpletype();
    bool     dest_derivs    = Destination.has_derivs();

    DASSERT (!Result.typespec().is_closure_based() &&
             !ObjectName.typespec().is_closure_based() && 
             !Attribute.typespec().is_closure_based() &&
             !Index.typespec().is_closure_based() && 
             !Destination.typespec().is_closure_based());

    // We'll pass the destination's attribute type directly to the 
    // RenderServices callback so that the renderer can perform any
    // necessary conversions from its internal format to OSL's.
    const TypeDesc* dest_type = &Destination.typespec().simpletype();

    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_constant ((int)dest_derivs));
    args.push_back (object_lookup ? rop.llvm_load_value (ObjectName) :
                                    rop.llvm_constant (ustring()));
    args.push_back (rop.llvm_load_value (Attribute));
    args.push_back (rop.llvm_constant ((int)array_lookup));
    args.push_back (rop.llvm_load_value (Index));
    args.push_back (rop.llvm_constant_ptr ((void *) dest_type));
    args.push_back (rop.llvm_void_ptr (Destination));

    llvm::Value *r = rop.llvm_call_function ("osl_get_attribute", &args[0], args.size());
    rop.llvm_store_value (r, Result);

    return true;
}



LLVMGEN (llvm_gen_gettextureinfo)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 4);

    Symbol& Result   = *rop.opargsym (op, 0);
    Symbol& Filename = *rop.opargsym (op, 1);
    Symbol& Dataname = *rop.opargsym (op, 2);
    Symbol& Data     = *rop.opargsym (op, 3);

    DASSERT (!Result.typespec().is_closure_based() &&
             Filename.typespec().is_string() && 
             Dataname.typespec().is_string() &&
             !Data.typespec().is_closure_based() && 
             Result.typespec().is_int());

    std::vector<llvm::Value *> args;

    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_load_value (Filename));
    args.push_back (rop.llvm_load_value (Dataname));
    // this is passes a TypeDesc to an LLVM op-code
    args.push_back (rop.llvm_constant((int) Data.typespec().simpletype().basetype));
    args.push_back (rop.llvm_constant((int) Data.typespec().simpletype().arraylen));
    args.push_back (rop.llvm_constant((int) Data.typespec().simpletype().aggregate));
    // destination
    args.push_back (rop.llvm_void_ptr (Data));

    llvm::Value *r = rop.llvm_call_function ("osl_get_textureinfo", &args[0], args.size());
    rop.llvm_store_value (r, Result);

    return true;
}



LLVMGEN (llvm_gen_getmessage)
{
    // getmessage() has four "flavors":
    //   * getmessage (attribute_name, value)
    //   * getmessage (attribute_name, value[])
    //   * getmessage (source, attribute_name, value)
    //   * getmessage (source, attribute_name, value[])
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 3 || op.nargs() == 4);
    int has_source = (op.nargs() == 4);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Source = *rop.opargsym (op, 1);
    Symbol& Name   = *rop.opargsym (op, 1+has_source);
    Symbol& Data   = *rop.opargsym (op, 2+has_source);
    DASSERT (Result.typespec().is_int() && Name.typespec().is_string());
    DASSERT (has_source == 0 || Source.typespec().is_string());

    llvm::Value *args[9];
    args[0] = rop.sg_void_ptr();
    args[1] = has_source ? rop.llvm_load_value(Source) 
                         : rop.llvm_constant(ustring());
    args[2] = rop.llvm_load_value (Name);

    if (Data.typespec().is_closure_based()) {
        // FIXME: secret handshake for closures ...
        args[3] = rop.llvm_constant (TypeDesc(TypeDesc::UNKNOWN,
                                              Data.typespec().arraylength()));
        // We need a void ** here so the function can modify the closure
        args[4] = rop.llvm_void_ptr(Data);
    } else {
        args[3] = rop.llvm_constant (Data.typespec().simpletype());
        args[4] = rop.llvm_void_ptr (Data);
    }
    args[5] = rop.llvm_constant ((int)Data.has_derivs());

    args[6] = rop.llvm_constant(rop.inst()->id());
    args[7] = rop.llvm_constant(op.sourcefile());
    args[8] = rop.llvm_constant(op.sourceline());

    llvm::Value *r = rop.llvm_call_function ("osl_getmessage", args, 9);
    rop.llvm_store_value (r, Result);
    return true;
}



LLVMGEN (llvm_gen_setmessage)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 2);
    Symbol& Name   = *rop.opargsym (op, 0);
    Symbol& Data   = *rop.opargsym (op, 1);
    DASSERT (Name.typespec().is_string());

    llvm::Value *args[7];
    args[0] = rop.sg_void_ptr();
    args[1] = rop.llvm_load_value (Name);
    if (Data.typespec().is_closure_based()) {
        // FIXME: secret handshake for closures ...
        args[2] = rop.llvm_constant (TypeDesc(TypeDesc::UNKNOWN,
                                              Data.typespec().arraylength()));
        // We need a void ** here so the function can modify the closure
        args[3] = rop.llvm_void_ptr(Data);
    } else {
        args[2] = rop.llvm_constant (Data.typespec().simpletype());
        args[3] = rop.llvm_void_ptr (Data);
    }

    args[4] = rop.llvm_constant(rop.inst()->id());
    args[5] = rop.llvm_constant(op.sourcefile());
    args[6] = rop.llvm_constant(op.sourceline());

    rop.llvm_call_function ("osl_setmessage", args, 7);
    return true;
}



LLVMGEN (llvm_gen_get_simple_SG_field)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 1);

    Symbol& Result = *rop.opargsym (op, 0);
    int sg_index = rop.ShaderGlobalNameToIndex (op.opname());
    ASSERT (sg_index >= 0);
    llvm::Value *sg_field = rop.builder().CreateConstGEP2_32 (rop.sg_ptr(), 0, sg_index);
    llvm::Value* r = rop.builder().CreateLoad(sg_field);
    rop.llvm_store_value (r, Result);

    return true;
}



LLVMGEN (llvm_gen_calculatenormal)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 2);

    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& P      = *rop.opargsym (op, 1);

    DASSERT (Result.typespec().is_triple() && P.typespec().is_triple());
    if (! P.has_derivs()) {
        rop.llvm_assign_zero (Result);
        return true;
    }
    
    std::vector<llvm::Value *> args;
    args.push_back (rop.llvm_void_ptr (Result));
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_void_ptr (P));
    rop.llvm_call_function ("osl_calculatenormal", &args[0], args.size());
    if (Result.has_derivs())
        rop.llvm_zero_derivs (Result);
    return true;
}



LLVMGEN (llvm_gen_area)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() == 2);

    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& P      = *rop.opargsym (op, 1);

    DASSERT (Result.typespec().is_float() && P.typespec().is_triple());
    if (! P.has_derivs()) {
        rop.llvm_assign_zero (Result);
        return true;
    }
    
    llvm::Value *r = rop.llvm_call_function ("osl_area", rop.llvm_void_ptr (P));
    rop.llvm_store_value (r, Result);
    if (Result.has_derivs())
        rop.llvm_zero_derivs (Result);
    return true;
}



LLVMGEN (llvm_gen_spline)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() >= 4 && op.nargs() <= 5);

    bool has_knot_count = (op.nargs() == 5);
    Symbol& Result   = *rop.opargsym (op, 0);
    Symbol& Spline   = *rop.opargsym (op, 1);
    Symbol& Value    = *rop.opargsym (op, 2);
    Symbol& Knot_count = *rop.opargsym (op, 3); // might alias Knots
    Symbol& Knots    = has_knot_count ? *rop.opargsym (op, 4) :
                                        *rop.opargsym (op, 3);

    DASSERT (!Result.typespec().is_closure_based() &&
             Spline.typespec().is_string()  && 
             Value.typespec().is_float() &&
             !Knots.typespec().is_closure_based() &&
             Knots.typespec().is_array() &&  
             (!has_knot_count || (has_knot_count && Knot_count.typespec().is_int())));

    std::string name = Strutil::format("osl_%s_", op.opname().c_str());
    std::vector<llvm::Value *> args;
    // only use derivatives for result if:
    //   result has derivs and (value || knots) have derivs
    bool result_derivs = Result.has_derivs() && (Value.has_derivs() || Knots.has_derivs());

    if (result_derivs)
        name += "d";
    if (Result.typespec().is_float())
        name += "f";
    else if (Result.typespec().is_triple())
        name += "v";

    if (result_derivs && Value.has_derivs())
        name += "d";
    if (Value.typespec().is_float())
        name += "f";
    else if (Value.typespec().is_triple())
        name += "v";

    if (result_derivs && Knots.has_derivs())
        name += "d";
    if (Knots.typespec().simpletype().elementtype() == TypeDesc::FLOAT)
        name += "f";
    else if (Knots.typespec().simpletype().elementtype().aggregate == TypeDesc::VEC3)
        name += "v";

    args.push_back (rop.llvm_void_ptr (Result));
    args.push_back (rop.llvm_load_value (Spline));
    args.push_back (rop.llvm_void_ptr (Value)); // make things easy
    args.push_back (rop.llvm_void_ptr (Knots));
    if (has_knot_count)
        args.push_back (rop.llvm_load_value (Knot_count));
    else
        args.push_back (rop.llvm_constant ((int)Knots.typespec().arraylength()));
    rop.llvm_call_function (name.c_str(), &args[0], args.size());

    if (Result.has_derivs() && !result_derivs)
        rop.llvm_zero_derivs (Result);

    return true;
}



static void
llvm_gen_keyword_fill(RuntimeOptimizer &rop, Opcode &op, const ClosureRegistry::ClosureEntry *clentry, ustring clname, llvm::Value *attr_p, int argsoffset)
{
    DASSERT(((op.nargs() - argsoffset) % 2) == 0);

    int Nattrs = (op.nargs() - argsoffset) / 2;

    for (int attr_i = 0; attr_i < Nattrs; ++attr_i) {
        int argno = attr_i * 2 + argsoffset;;
        Symbol &Key     = *rop.opargsym (op, argno);
        Symbol &Value   = *rop.opargsym (op, argno + 1);
        ASSERT(Key.typespec().is_string());
        ASSERT(Key.is_constant());
        ustring *key = (ustring *)Key.data();
        TypeDesc ValueType = Value.typespec().simpletype();

        bool legal = false;
        // Make sure there is some keyword arg that has the name and the type
        for (int t = 0; t < clentry->nkeyword; ++t) {
            const ClosureParam &param = clentry->params[clentry->nformal + t];
            // strcmp might be too much, we could precompute the ustring for the param,
            // but in this part of the code is not a big deal
            if (param.type == ValueType && !strcmp(key->c_str(), param.key))
                legal = true;
        }
        if (!legal) {
            rop.shadingsys().warning("Unsupported closure keyword arg \"%s\" for %s (%s:%d)", key->c_str(), clname.c_str(), op.sourcefile().c_str(), op.sourceline());
            continue;
        }

        llvm::Value *key_to     = rop.builder().CreateConstGEP2_32 (attr_p, attr_i, 0);
        llvm::Value *key_const  = rop.llvm_constant_ptr(*((void **)key), rop.llvm_type_string());
        llvm::Value *value_to   = rop.builder().CreateConstGEP2_32 (attr_p, attr_i, 1);
        llvm::Value *value_from = rop.llvm_void_ptr (Value);
        value_to = rop.llvm_ptr_cast (value_to, rop.llvm_type_void_ptr());

        rop.builder().CreateStore (key_const, key_to);
        rop.llvm_memcpy (value_to, value_from, (int)ValueType.size(), 4);
    }
}



LLVMGEN (llvm_gen_closure)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    ASSERT (op.nargs() >= 2); // at least the result and the ID

    Symbol &Result   = *rop.opargsym (op, 0);
    Symbol &Id       = *rop.opargsym (op, 1);
    DASSERT(Result.typespec().is_closure());
    DASSERT(Id.typespec().is_string());
    ustring closure_name = *((ustring *)Id.data());

    const ClosureRegistry::ClosureEntry * clentry = rop.shadingsys().find_closure(closure_name);
    if (!clentry) {
        rop.shadingsys().error ("Closure '%s' is not supported by the current renderer, called from (%s:%d)",
                                closure_name.c_str(), op.sourcefile().c_str(), op.sourceline());
        return false;
    }

    ASSERT (op.nargs() >= (2 + clentry->nformal));
    int nattrs = (op.nargs() - (2 + clentry->nformal)) / 2;

    // Call osl_allocate_closure_component(closure, id, size).  It returns
    // the memory for the closure parameter data.
    llvm::Value *render_ptr = rop.llvm_constant_ptr(rop.shadingsys().renderer(), rop.llvm_type_void_ptr());
    llvm::Value *sg_ptr = rop.sg_void_ptr();
    llvm::Value *id_int = rop.llvm_constant(clentry->id);
    llvm::Value *size_int = rop.llvm_constant(clentry->struct_size);
    llvm::Value *nattrs_int = rop.llvm_constant(nattrs);
    llvm::Value *alloc_args[4] = {sg_ptr, id_int, size_int, nattrs_int};
    llvm::Value *comp_void_ptr = rop.llvm_call_function ("osl_allocate_closure_component", alloc_args, 4);
    rop.llvm_store_value (comp_void_ptr, Result, 0, NULL, 0);
    llvm::Value *comp_ptr = rop.llvm_ptr_cast(comp_void_ptr, rop.llvm_type_closure_component_ptr());
    // Get the address of the primitive buffer, which is the 5th field
    llvm::Value *mem_void_ptr = rop.builder().CreateConstGEP2_32 (comp_ptr, 0, 4);
    mem_void_ptr = rop.llvm_ptr_cast(mem_void_ptr, rop.llvm_type_void_ptr());

    // If the closure has a "prepare" method, call
    // prepare(renderer, id, memptr).  If there is no prepare method, just
    // zero out the closure parameter memory.
    if (clentry->prepare) {
        // Call clentry->prepare(renderservices *, int id, void *mem)
        llvm::Value *funct_ptr = rop.llvm_constant_ptr((void *)clentry->prepare, rop.llvm_type_prepare_closure_func());
        llvm::Value *args[3] = {render_ptr, id_int, mem_void_ptr};
        rop.llvm_call_function (funct_ptr, args, 3);
    } else {
        rop.llvm_memset (mem_void_ptr, 0, clentry->struct_size, 4 /*align*/);
    }

    // Here is where we fill the struct using the params
    for (int carg = 0; carg < clentry->nformal; ++carg) {
        const ClosureParam &p = clentry->params[carg];
        if (p.key != NULL) break;
        ASSERT(p.offset < clentry->struct_size);
        Symbol &sym = *rop.opargsym (op, carg + 2);
        TypeDesc t = sym.typespec().simpletype();
        if (t.vecsemantics == TypeDesc::NORMAL || t.vecsemantics == TypeDesc::POINT)
            t.vecsemantics = TypeDesc::VECTOR;
        if (!sym.typespec().is_closure_based() && !sym.typespec().is_structure() && t == p.type) {
            llvm::Value* dst = rop.llvm_offset_ptr (mem_void_ptr, p.offset);
            llvm::Value* src = rop.llvm_void_ptr (sym);
            rop.llvm_memcpy (dst, src, (int)p.type.size(),
                             4 /* use 4 byte alignment for now */);
        } else {
            rop.shadingsys().error ("Incompatible formal argument %d to '%s' closure. Prototypes don't match renderer registry.",
                                    carg + 1, closure_name.c_str());
        }
    }

    // If the closure has a "setup" method, call
    // setup(render_services, id, mem_ptr).
    if (clentry->setup) {
        // Call clentry->setup(renderservices *, int id, void *mem)
        llvm::Value *funct_ptr = rop.llvm_constant_ptr((void *)clentry->setup, rop.llvm_type_setup_closure_func());
        llvm::Value *args[3] = {render_ptr, id_int, mem_void_ptr};
        rop.llvm_call_function (funct_ptr, args, 3);
    }

    llvm::Value *attrs_void_ptr = rop.llvm_offset_ptr (mem_void_ptr, clentry->struct_size);
    llvm::Value *attrs_ptr = rop.llvm_ptr_cast(attrs_void_ptr, rop.llvm_type_closure_component_attr_ptr());
    llvm_gen_keyword_fill(rop, op, clentry, closure_name, attrs_ptr, clentry->nformal + 2);

    return true;
}



LLVMGEN (llvm_gen_pointcloud_search)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() >= 5);
    // Does the compiler check this? Can we turn it
    // into a DASSERT?
    ASSERT (((op.nargs() - 5) % 2) == 0);

    Symbol& Result     = *rop.opargsym (op, 0);
    Symbol& Filename   = *rop.opargsym (op, 1);
    Symbol& Center     = *rop.opargsym (op, 2);
    Symbol& Radius     = *rop.opargsym (op, 3);
    Symbol& Max_points = *rop.opargsym (op, 4);

    DASSERT (Result.typespec().is_int() && Filename.typespec().is_string() &&
             Center.typespec().is_triple() && Radius.typespec().is_float() &&
             Max_points.typespec().is_int());


    std::vector<Symbol *> clear_derivs_of; // arguments whose derivs we need to zero at the end
    int attr_arg_offset = 5; // where the opt attrs begin
    int nattrs = (op.nargs() - attr_arg_offset) / 2;

    static ustring u_distance ("distance");
    static ustring u_index ("index");

    std::vector<llvm::Value *> args;
    args.push_back (rop.sg_void_ptr());                // 0 sg
    args.push_back (rop.llvm_load_value (Filename));   // 1 filename
    args.push_back (rop.llvm_void_ptr   (Center));     // 2 center
    args.push_back (rop.llvm_load_value (Radius));     // 3 radius
    args.push_back (rop.llvm_load_value (Max_points)); // 4 max_points
    args.push_back (rop.llvm_constant_ptr (NULL));      // 5 indices
    args.push_back (rop.llvm_constant_ptr (NULL));      // 6 distances
    args.push_back (rop.llvm_constant (0));             // 7 derivs_offset
    args.push_back (NULL);                             // 8 nattrs
    size_t capacity = 0x7FFFFFFF; // Lets put a 32 bit limit
    int extra_attrs = 0; // Extra query attrs to search
    // This loop does three things. 1) Look for the special attributes
    // "distance", "index" and grab the pointer. 2) Compute the minimmum
    // size of the provided output arrays to check against max_points
    // 3) push optional args to the arg list
    for (int i = 0; i < nattrs; ++i) {
        Symbol& Name  = *rop.opargsym (op, attr_arg_offset + i*2);
        Symbol& Value = *rop.opargsym (op, attr_arg_offset + i*2 + 1);

        ASSERT (Name.typespec().is_string());
        TypeDesc simpletype = Value.typespec().simpletype();
        if (Name.is_constant() && *((ustring *)Name.data()) == u_index &&
            simpletype.elementtype() == TypeDesc::INT) {
            args[5] = rop.llvm_void_ptr (Value);
        } else if (Name.is_constant() && *((ustring *)Name.data()) == u_distance &&
                   simpletype.elementtype() == TypeDesc::FLOAT) {
            args[6] = rop.llvm_void_ptr (Value);
            if (Value.has_derivs()) {
                if (Center.has_derivs())
                    // deriv offset is the size of the array
                    args[7] = rop.llvm_constant ((int)simpletype.numelements());
                else
                    clear_derivs_of.push_back(&Value);
            }
        } else {
            // It is a regular attribute, push it to the arg list
            args.push_back (rop.llvm_load_value (Name));
            args.push_back (rop.llvm_constant (simpletype));
            args.push_back (rop.llvm_void_ptr (Value));
            if (Value.has_derivs())
                clear_derivs_of.push_back(&Value);
            extra_attrs++;
        }
        // minimum capacity of the output arrays
        capacity = std::min (simpletype.numelements(), capacity);
    }

    args[8] = rop.llvm_constant (extra_attrs);

    // Compare capacity to the requested number of points. The available
    // space on the arrays is a constant, the requested number of
    // points is not, so runtime check.
    llvm::Value *sizeok = rop.builder().CreateICmpSGE (rop.llvm_constant((int)capacity), args[4]); // max_points

    llvm::BasicBlock* sizeok_block = rop.llvm_new_basic_block ("then");
    llvm::BasicBlock* badsize_block = rop.llvm_new_basic_block ("else");
    llvm::BasicBlock* after_block = rop.llvm_new_basic_block ("");
    rop.builder().CreateCondBr (sizeok, sizeok_block, badsize_block);

    // non-error code
    rop.builder().SetInsertPoint (sizeok_block);

    llvm::Value *count = rop.llvm_call_function ("osl_pointcloud_search", &args[0], args.size());
    // Clear derivs if necessary
    for (size_t i = 0; i < clear_derivs_of.size(); ++i)
        rop.llvm_zero_derivs (*clear_derivs_of[i], count);
    // Store result
    rop.llvm_store_value (count, Result);

    // error code
    rop.builder().CreateBr (after_block);
    rop.builder().SetInsertPoint (badsize_block);

    args.clear();
    static ustring errorfmt("Arrays too small for pointcloud lookup at (%s:%d)");

    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_constant_ptr ((void *)errorfmt.c_str()));
    args.push_back (rop.llvm_constant_ptr ((void *)op.sourcefile().c_str()));
    args.push_back (rop.llvm_constant (op.sourceline()));
    rop.llvm_call_function ("osl_error", &args[0], args.size());

    rop.builder().CreateBr (after_block);
    rop.builder().SetInsertPoint (after_block);
    return true;
}



LLVMGEN (llvm_gen_pointcloud_get)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    DASSERT (op.nargs() >= 6);

    Symbol& Result     = *rop.opargsym (op, 0);
    Symbol& Filename   = *rop.opargsym (op, 1);
    Symbol& Indices    = *rop.opargsym (op, 2);
    Symbol& Count      = *rop.opargsym (op, 3);
    Symbol& Attr_name  = *rop.opargsym (op, 4);
    Symbol& Data       = *rop.opargsym (op, 5);

    llvm::Value *count = rop.llvm_load_value (Count);

    int capacity = std::min ((int)Data.typespec().simpletype().numelements(), (int)Indices.typespec().simpletype().numelements());
    // Check available space
    llvm::Value *sizeok = rop.builder().CreateICmpSGE (rop.llvm_constant(capacity), count);

    llvm::BasicBlock* sizeok_block = rop.llvm_new_basic_block ("then");
    llvm::BasicBlock* badsize_block = rop.llvm_new_basic_block ("else");
    llvm::BasicBlock* after_block = rop.llvm_new_basic_block ("");
    rop.builder().CreateCondBr (sizeok, sizeok_block, badsize_block);

    // non-error code
    rop.builder().SetInsertPoint (sizeok_block);

    // Convert 32bit indices to 64bit
    std::vector<llvm::Value *> args;

    args.clear();
    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_load_value (Filename));
    args.push_back (rop.llvm_void_ptr (Indices));
    args.push_back (count);
    args.push_back (rop.llvm_load_value (Attr_name));
    args.push_back (rop.llvm_constant (Data.typespec().simpletype()));
    args.push_back (rop.llvm_void_ptr (Data));
    llvm::Value *found = rop.llvm_call_function ("osl_pointcloud_get", &args[0], args.size());
    rop.llvm_store_value (found, Result);
    if (Data.has_derivs())
        rop.llvm_zero_derivs (Data, count);

    // error code
    rop.builder().CreateBr (after_block);
    rop.builder().SetInsertPoint (badsize_block);

    args.clear();
    static ustring errorfmt("Arrays too small for pointcloud attribute get at (%s:%d)");

    args.push_back (rop.sg_void_ptr());
    args.push_back (rop.llvm_constant_ptr ((void *)errorfmt.c_str()));
    args.push_back (rop.llvm_constant_ptr ((void *)op.sourcefile().c_str()));
    args.push_back (rop.llvm_constant (op.sourceline()));
    rop.llvm_call_function ("osl_error", &args[0], args.size());

    rop.builder().CreateBr (after_block);
    rop.builder().SetInsertPoint (after_block);
    return true;
}



LLVMGEN (llvm_gen_dict_find)
{
    // OSL has two variants of this function:
    //     dict_find (string dict, string query)
    //     dict_find (int nodeID, string query)
    Opcode &op (rop.inst()->ops()[opnum]);
    DASSERT (op.nargs() == 3);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Source = *rop.opargsym (op, 1);
    Symbol& Query  = *rop.opargsym (op, 2);
    DASSERT (Result.typespec().is_int() && Query.typespec().is_string() &&
             (Source.typespec().is_int() || Source.typespec().is_string()));
    bool sourceint = Source.typespec().is_int();  // is it an int?
    llvm::Value *args[3];
    args[0] = rop.sg_void_ptr();
    args[1] = rop.llvm_load_value(Source);
    args[2] = rop.llvm_load_value (Query);
    const char *func = sourceint ? "osl_dict_find_iis" : "osl_dict_find_iss";
    llvm::Value *ret = rop.llvm_call_function (func, &args[0], 3);
    rop.llvm_store_value (ret, Result);
    return true;
}



LLVMGEN (llvm_gen_dict_next)
{
    // dict_net is very straightforward -- just insert sg ptr as first arg
    Opcode &op (rop.inst()->ops()[opnum]);
    DASSERT (op.nargs() == 2);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& NodeID = *rop.opargsym (op, 1);
    DASSERT (Result.typespec().is_int() && NodeID.typespec().is_int());
    llvm::Value *ret = rop.llvm_call_function ("osl_dict_next",
                                               rop.sg_void_ptr(),
                                               rop.llvm_load_value(NodeID));
    rop.llvm_store_value (ret, Result);
    return true;
}



LLVMGEN (llvm_gen_dict_value)
{
    // int dict_value (int nodeID, string attribname, output TYPE value)
    Opcode &op (rop.inst()->ops()[opnum]);
    DASSERT (op.nargs() == 4);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& NodeID = *rop.opargsym (op, 1);
    Symbol& Name   = *rop.opargsym (op, 2);
    Symbol& Value  = *rop.opargsym (op, 3);
    DASSERT (Result.typespec().is_int() && NodeID.typespec().is_int() &&
             Name.typespec().is_string());
    llvm::Value *args[5];
    // arg 0: shaderglobals ptr
    args[0] = rop.sg_void_ptr();
    // arg 1: nodeID
    args[1] = rop.llvm_load_value(NodeID);
    // arg 2: attribute name
    args[2] = rop.llvm_load_value(Name);
    // arg 3: encoded type of Value
    args[3] = rop.llvm_constant(Value.typespec().simpletype());
    // arg 4: pointer to Value
    args[4] = rop.llvm_void_ptr (Value);
    llvm::Value *ret = rop.llvm_call_function ("osl_dict_value", &args[0], 5);
    rop.llvm_store_value (ret, Result);
    return true;
}



LLVMGEN (llvm_gen_raytype)
{
    // int raytype (string name)
    Opcode &op (rop.inst()->ops()[opnum]);
    DASSERT (op.nargs() == 2);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Name = *rop.opargsym (op, 1);
    llvm::Value *args[2] = { rop.sg_void_ptr(), NULL };
    const char *func = NULL;
    if (Name.is_constant()) {
        // We can statically determine the bit pattern
        ustring name = ((ustring *)Name.data())[0];
        args[1] = rop.llvm_constant (rop.shadingsys().raytype_bit (name));
        func = "osl_raytype_bit";
    } else {
        // No way to know which name is being asked for
        args[1] = rop.llvm_get_pointer (Name);
        func = "osl_raytype_name";
    }
    llvm::Value *ret = rop.llvm_call_function (func, args, 2);
    rop.llvm_store_value (ret, Result);
    return true;
}



// color blackbody (float temperatureK)
// color wavelength_color (float wavelength_nm)  // same function signature
LLVMGEN (llvm_gen_blackbody)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    ASSERT (op.nargs() == 2);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &Temperature (*rop.opargsym (op, 1));
    ASSERT (Result.typespec().is_triple() && Temperature.typespec().is_float());

    llvm::Value* args[3] = { rop.sg_void_ptr(), rop.llvm_void_ptr(Result),
                             rop.llvm_load_value(Temperature) };
    rop.llvm_call_function (Strutil::format("osl_%s_vf",op.opname().c_str()).c_str(), args, 3);

    // Punt, zero out derivs.
    // FIXME -- only of some day, someone truly needs blackbody() to
    // correctly return derivs with spatially-varying temperature.
    if (Result.has_derivs())
        rop.llvm_zero_derivs (Result);

    return true;
}



// float luminance (color c)
LLVMGEN (llvm_gen_luminance)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    ASSERT (op.nargs() == 2);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &C (*rop.opargsym (op, 1));
    ASSERT (Result.typespec().is_float() && C.typespec().is_triple());

    bool deriv = C.has_derivs() && Result.has_derivs();
    llvm::Value* args[3] = { rop.sg_void_ptr(), rop.llvm_void_ptr(Result),
                             rop.llvm_void_ptr(C) };
    rop.llvm_call_function (deriv ? "osl_luminance_dvdf" : "osl_luminance_fv",
                            args, 3);

    if (Result.has_derivs() && !C.has_derivs())
        rop.llvm_zero_derivs (Result);

    return true;
}



LLVMGEN (llvm_gen_functioncall)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    ASSERT (op.nargs() == 1);

    llvm::BasicBlock* after_block = rop.llvm_new_basic_block ("");
    rop.llvm_push_function (after_block);

    // Generate the code for the body of the function
    rop.build_llvm_code (opnum+1, op.jump(0));
    rop.builder().CreateBr (after_block);

    // Continue on with the previous flow
    rop.builder().SetInsertPoint (after_block);
    rop.llvm_pop_function ();

    return true;
}



LLVMGEN (llvm_gen_return)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    ASSERT (op.nargs() == 0);
    rop.builder().CreateBr (rop.llvm_return_block());
    llvm::BasicBlock* next_block = rop.llvm_new_basic_block ("");
    rop.builder().SetInsertPoint (next_block);
    return true;
}



}; // namespace pvt
}; // namespace osl

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif
