/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/fresh_name.h"
#include "util/sstream.h"
#include "kernel/instantiate.h"
#include "kernel/inductive/inductive.h"
#include "library/constants.h"
#include "library/trace.h"
#include "library/annotation.h"
#include "library/util.h"
#include "library/quote.h"
#include "library/vm/vm.h"
#include "library/vm/optimize.h"
#include "library/compiler/simp_inductive.h"
#include "library/compiler/erase_irrelevant.h"
#include "library/compiler/nat_value.h"
#include "library/compiler/preprocess.h"

namespace lean {
class vm_compiler_fn {
    environment        m_env;
    buffer<vm_instr> & m_code;

    void emit(vm_instr const & i) {
        m_code.push_back(i);
    }

    unsigned next_pc() const {
        return m_code.size();
    }

    expr mk_local(name const & n) {
        return ::lean::mk_local(n, mk_neutral_expr());
    }

    void compile_args(unsigned nargs, expr const * args, unsigned bpz, name_map<unsigned> const & m) {
        for (unsigned i = 0; i < nargs; i++, bpz++) {
            compile(args[i], bpz, m);
        }
    }

    void compile_rev_args(unsigned nargs, expr const * args, unsigned bpz, name_map<unsigned> const & m) {
        unsigned i = nargs;
        while (i > 0) {
            --i;
            compile(args[i], bpz, m);
            bpz++;
        }
    }

    void compile_global(vm_decl const & decl, unsigned nargs, expr const * args, unsigned bpz, name_map<unsigned> const & m) {
        compile_rev_args(nargs, args, bpz, m);
        if (decl.get_arity() <= nargs) {
            if (decl.is_builtin())
                emit(mk_invoke_builtin_instr(decl.get_idx()));
            else if (decl.is_cfun())
                emit(mk_invoke_cfun_instr(decl.get_idx()));
            else
                emit(mk_invoke_global_instr(decl.get_idx()));
            emit_apply_instr(nargs - decl.get_arity());
        } else {
            lean_assert(decl.get_arity() > nargs);
            emit(mk_closure_instr(decl.get_idx(), nargs));
        }
    }

    [[ noreturn ]] void throw_unknown_constant(name const & n) {
        throw exception(sstream() << "code generation failed, VM does not have code for '" << n << "'");
    }

    void emit_apply_instr(unsigned n) {
        for (unsigned i = 0; i < n; i++)
            emit(mk_apply_instr());
    }

    void compile_constant(expr const & e) {
        name const & n = const_name(e);
        if (is_neutral_expr(e)) {
            emit(mk_sconstructor_instr(0));
        } else if (is_unreachable_expr(e)) {
            emit(mk_unreachable_instr());
        } else if (n == get_nat_zero_name()) {
            emit(mk_num_instr(mpz(0)));
        } else if (auto idx = is_internal_cnstr(e)) {
            emit(mk_sconstructor_instr(*idx));
        } else if (optional<vm_decl> decl = get_vm_decl(m_env, n)) {
            compile_global(*decl, 0, nullptr, 0, name_map<unsigned>());
        } else {
            throw_unknown_constant(n);
        }
    }

    void compile_local(expr const & e, name_map<unsigned> const & m) {
        unsigned idx = *m.find(mlocal_name(e));
        emit(mk_push_instr(idx));
    }

    void compile_cases_on(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        buffer<expr> args;
        expr fn = get_app_args(e, args);
        lean_assert(is_constant(fn));
        name const & fn_name = const_name(fn);
        unsigned num;
        optional<unsigned> builtin_cases_idx;
        if (fn_name == get_nat_cases_on_name()) {
            num = 2;
        } else {
            builtin_cases_idx = get_vm_builtin_cases_idx(m_env, fn_name);
            if (builtin_cases_idx) {
                name const & I_name = fn_name.get_prefix();
                num = *inductive::get_num_intro_rules(m_env, I_name);
            } else {
                lean_assert(is_internal_cases(fn));
                num = *is_internal_cases(fn);
            }
        }
        lean_assert(args.size() == num + 1);
        lean_assert(num >= 1);
        /** compile major premise */
        compile(args[0], bpz, m);
        unsigned cases_pos = next_pc();
        buffer<unsigned> cases_args;
        buffer<unsigned> goto_pcs;
        cases_args.resize(num, 0);
        if (fn_name == get_nat_cases_on_name()) {
            emit(mk_nat_cases_instr(0, 0));
        } else if (builtin_cases_idx) {
            #if defined(__GNUC__) && !defined(__CLANG__)
            #pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
            #endif
            emit(mk_builtin_cases_instr(*builtin_cases_idx, cases_args.size(), cases_args.data()));
        } else if (num == 1) {
            emit(mk_destruct_instr());
        } else if (num == 2) {
            emit(mk_cases2_instr(0, 0));
        } else {
            emit(mk_casesn_instr(cases_args.size(), cases_args.data()));
        }
        for (unsigned i = 1; i < args.size(); i++) {
            cases_args[i - 1] = next_pc();
            expr b = args[i];
            buffer<expr> locals;
            name_map<unsigned> new_m = m;
            unsigned new_bpz = bpz;
            while (is_lambda(b)) {
                name n = mk_fresh_name();
                new_m.insert(n, new_bpz);
                locals.push_back(mk_local(n));
                new_bpz++;
                b = binding_body(b);
            }
            b = instantiate_rev(b, locals.size(), locals.data());
            compile(b, new_bpz, new_m);
            if (locals.size() > 0)
                emit(mk_drop_instr(locals.size()));
            // if it is not the last case, we need to use a goto
            if (i + 1 < args.size()) {
                goto_pcs.push_back(next_pc());
                emit(mk_goto_instr(0)); // fix later
            }
        }
        /* Fix cases instruction pc's */
        if (num >= 2 || builtin_cases_idx) {
            for (unsigned i = 0; i < cases_args.size(); i++)
                m_code[cases_pos].set_pc(i, cases_args[i]);
        }
        unsigned end_pc = next_pc();
        /* Fix goto instruction pc's */
        for (unsigned i = 0; i < goto_pcs.size(); i++) {
            m_code[goto_pcs[i]].set_goto_pc(end_pc);
        }
    }

    void compile_cnstr(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        buffer<expr> args;
        expr const & fn = get_app_args(e, args);
        lean_assert(is_internal_cnstr(fn));
        unsigned cidx = *is_internal_cnstr(fn);
        compile_args(args.size(), args.data(), bpz, m);
        emit(mk_constructor_instr(cidx, get_app_num_args(e)));
    }

    void compile_proj(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        buffer<expr> args;
        expr const & fn = get_app_args(e, args);
        lean_assert(is_internal_proj(fn));
        unsigned idx = *is_internal_proj(fn);
        lean_assert(args.size() >= 1);
        compile_rev_args(args.size() - 1, args.data() + 1, bpz, m);
        bpz += args.size() - 1;
        compile(args[0], bpz, m);
        emit(mk_proj_instr(idx));
        emit_apply_instr(args.size() - 1);
    }

    void compile_fn_call(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        buffer<expr> args;
        expr fn = get_app_args(e, args);
        if (!is_constant(fn)) {
            compile_rev_args(args.size(), args.data(), bpz+1, m);
            compile(fn, bpz, m);
            emit_apply_instr(args.size());
            return;
        } else if (is_constant(fn)) {
            if (is_neutral_expr(fn)) {
                emit(mk_sconstructor_instr(0));
            } else if (optional<vm_decl> decl = get_vm_decl(m_env, const_name(fn))) {
                compile_global(*decl, args.size(), args.data(), bpz, m);
            } else {
                throw_unknown_constant(const_name(fn));
            }
        } else {
            lean_unreachable();
        }
    }

    bool is_builtin_cases(expr const & fn) {
        return is_constant(fn) && get_vm_builtin_cases_idx(m_env, const_name(fn));
    }

    void compile_app(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        expr const & fn = get_app_fn(e);
        if (is_internal_cases(fn) || is_constant(fn, get_nat_cases_on_name()) || is_builtin_cases(fn)) {
            compile_cases_on(e, bpz, m);
        } else if (is_internal_cnstr(fn)) {
            compile_cnstr(e, bpz, m);
        } else if (is_internal_proj(fn)) {
            compile_proj(e, bpz, m);
        } else {
            compile_fn_call(e, bpz, m);
        }
    }

    void compile_let(expr e, unsigned bpz, name_map<unsigned> const & m) {
        unsigned counter = 0;
        buffer<expr> locals;
        name_map<unsigned> new_m = m;
        while (is_let(e)) {
            counter++;
            compile(instantiate_rev(let_value(e), locals.size(), locals.data()), bpz, new_m);
            name n = mk_fresh_name();
            new_m.insert(n, bpz);
            locals.push_back(mk_local(n));
            bpz++;
            e = let_body(e);
        }
        lean_assert(counter > 0);
        compile(instantiate_rev(e, locals.size(), locals.data()), bpz, new_m);
        emit(mk_drop_instr(counter));
    }

    void compile_macro(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        if (is_nat_value(e)) {
            emit(mk_num_instr(get_nat_value_value(e)));
        } else if (is_annotation(e)) {
            compile(get_annotation_arg(e), bpz, m);
        } else if (is_quote(e)) {
            emit(mk_pexpr_instr(get_quote_expr(e)));
        } else {
            throw exception(sstream() << "code generation failed, unexpected kind of macro has been found: '"
                            << macro_def(e).get_name() << "'");
        }
    }

    void compile(expr const & e, unsigned bpz, name_map<unsigned> const & m) {
        switch (e.kind()) {
        case expr_kind::Var:      lean_unreachable();
        case expr_kind::Sort:     lean_unreachable();
        case expr_kind::Meta:     lean_unreachable();
        case expr_kind::Pi:       lean_unreachable();
        case expr_kind::Lambda:   lean_unreachable();
        case expr_kind::Macro:    compile_macro(e, bpz, m);  break;
        case expr_kind::Constant: compile_constant(e);       break;
        case expr_kind::Local:    compile_local(e, m);       break;
        case expr_kind::App:      compile_app(e, bpz, m);    break;
        case expr_kind::Let:      compile_let(e, bpz, m);    break;
        }
    }

    unsigned get_arity(expr e) {
        unsigned r = 0;
        while (is_lambda(e)) {
            r++;
            e = binding_body(e);
        }
        return r;
    }

public:
    vm_compiler_fn(environment const & env, buffer<vm_instr> & code):
        m_env(env), m_code(code) {}

    unsigned operator()(expr e) {
        buffer<expr> locals;
        unsigned bpz   = 0;
        unsigned arity = get_arity(e);
        unsigned i     = arity;
        name_map<unsigned> m;
        while (is_lambda(e)) {
            name n = mk_fresh_name();
            i--;
            m.insert(n, i);
            locals.push_back(mk_local(n));
            bpz++;
            e = binding_body(e);
        }
        e = instantiate_rev(e, locals.size(), locals.data());
        compile(e, bpz, m);
        emit(mk_ret_instr());
        return arity;
    }
};

environment vm_compile(environment const & env, buffer<pair<name, expr>> const & procs) {
    environment new_env = env;
    for (auto const & p : procs) {
        new_env = reserve_vm_index(new_env, p.first, p.second);
    }
    for (auto const & p : procs) {
        buffer<vm_instr> code;
        vm_compiler_fn gen(new_env, code);
        unsigned arity = gen(p.second);
        lean_trace(name({"compiler", "code_gen"}), tout() << " " << p.first << " " << arity << "\n";
                   display_vm_code(tout().get_stream(), new_env, code.size(), code.data()););
        optimize(new_env, code);
        lean_trace(name({"compiler", "optimize_bytecode"}), tout() << " " << p.first << " " << arity << "\n";
                   display_vm_code(tout().get_stream(), new_env, code.size(), code.data()););
        new_env = update_vm_code(new_env, p.first, code.size(), code.data());
    }
    return new_env;
}

environment vm_compile(environment const & env, declaration const & d) {
    buffer<pair<name, expr>> procs;
    preprocess(env, d, procs);
    return vm_compile(env, procs);
}

void initialize_vm_compiler() {
    register_trace_class({"compiler", "optimize_bytecode"});
    register_trace_class({"compiler", "code_gen"});
}

void finalize_vm_compiler() {
}
}
