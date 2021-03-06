/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "kernel/environment.h"

namespace lean {
environment vm_compile(environment const & env, buffer<pair<name, expr>> const & procs);
environment vm_compile(environment const & env, declaration const & d);
void initialize_vm_compiler();
void finalize_vm_compiler();
}
