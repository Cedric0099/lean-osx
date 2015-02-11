/*
Copyright (c) 2015 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/sstream.h"
#include "kernel/find_fn.h"
#include "kernel/instantiate.h"
#include "library/scoped_ext.h"
#include "library/expr_lt.h"
#include "library/util.h"
#include "library/replace_visitor.h"

namespace lean {
typedef pair<name, bool> abbrev_entry;

struct abbrev_state {
    name_map<bool>                               m_abbrevs;
    rb_map<expr, name, expr_cmp_no_level_params> m_inv_map; // for pretty printer

    void add(environment const & env, name const & n, bool parsing_only) {
        declaration const & d = env.get(n);
        if (!d.is_definition())
            throw exception(sstream() << "invalid abbreviation '" << n << "', it is not a definition");
        m_abbrevs.insert(n, parsing_only);
        if (!parsing_only) {
            m_inv_map.insert(d.get_value(), n);
        }
    }

    bool is_abbreviation(name const & n) const { return m_abbrevs.contains(n); }

    bool is_parsing_only_abbreviation(name const & n) const {
        if (auto it = m_abbrevs.find(n))
            return *it;
        else
            return false;
    }

    optional<name> is_abbreviated(expr const & e) const {
        if (auto it = m_inv_map.find(e))
            return optional<name>(*it);
        else
            return optional<name>();
    }
};

static name * g_class_name = nullptr;
static std::string * g_key = nullptr;

struct abbrev_config {
    typedef abbrev_state  state;
    typedef abbrev_entry  entry;
    static void add_entry(environment const & env, io_state const &, state & s, entry const & e) {
        s.add(env, e.first, e.second);
    }
    static name const & get_class_name() {
        return *g_class_name;
    }
    static std::string const & get_serialization_key() {
        return *g_key;
    }
    static void  write_entry(serializer & s, entry const & e) {
        s << e.first << e.second;
    }
    static entry read_entry(deserializer & d) {
        entry e;
        d >> e.first >> e.second;
        return e;
    }
    static optional<unsigned> get_fingerprint(entry const & e) {
        return some(e.first.hash());
    }
};

template class scoped_ext<abbrev_config>;
typedef scoped_ext<abbrev_config> abbrev_ext;

environment add_abbreviation(environment const & env, name const & n, bool parsing_only, bool persistent) {
    return abbrev_ext::add_entry(env, get_dummy_ios(), abbrev_entry(n, parsing_only), persistent);
}

bool is_abbreviation(environment const & env, name const & n) {
    abbrev_state const & s = abbrev_ext::get_state(env);
    return s.is_abbreviation(n);
}

bool is_parsing_only_abbreviation(environment const & env, name const & n) {
    abbrev_state const & s = abbrev_ext::get_state(env);
    return s.is_parsing_only_abbreviation(n);
}

optional<name> is_abbreviated(environment const & env, expr const & e) {
    abbrev_state const & s = abbrev_ext::get_state(env);
    return s.is_abbreviated(e);
}

bool contains_abbreviations(environment const & env, expr const & e) {
    abbrev_state const & s = abbrev_ext::get_state(env);
    return static_cast<bool>(find(e, [&](expr const & e, unsigned) {
                return is_constant(e) && s.is_abbreviation(const_name(e));
            }));
}

struct expand_abbrev_fn : public replace_visitor {
    environment const &  m_env;
    abbrev_state const & m_state;

    virtual expr visit_app(expr const & e) {
        buffer<expr> args;
        expr const & f = get_app_args(e, args);
        expr new_f     = visit(f);
        bool modified  = !is_eqp(f, new_f);
        for (expr & arg : args) {
            expr new_arg = visit(arg);
            if (!is_eqp(arg, new_arg))
                modified = true;
            arg = new_arg;
        }
        expr r;
        if (modified)
            r = mk_app(new_f, args);
        else
            r = e;
        if (is_constant(new_f) && m_state.is_abbreviation(const_name(new_f)))
            r = *unfold_app(m_env, r);
        return r;
    }

    virtual expr visit_constant(expr const & e) {
        if (m_state.is_abbreviation(const_name(e)))
            return instantiate_value_univ_params(m_env.get(const_name(e)), const_levels(e));
        else
            return e;
    }

    expand_abbrev_fn(environment const & env):
        m_env(env),
        m_state(abbrev_ext::get_state(env)) {
    }
};

expr expand_abbreviations(environment const & env, expr const & e) {
    if (!contains_abbreviations(env, e))
        return e;
    return expand_abbrev_fn(env)(e);
}

void initialize_abbreviation() {
    g_class_name = new name("abbreviations");
    g_key        = new std::string("abbrev");
    abbrev_ext::initialize();
}

void finalize_abbreviation() {
    abbrev_ext::finalize();
    delete g_key;
    delete g_class_name;
}
}
