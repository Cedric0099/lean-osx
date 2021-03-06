/*
Copyright (c) 2015 Daniel Selsam. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Author: Daniel Selsam
*/
#include "library/tactic/backward/init_module.h"
#include "library/tactic/backward/backward_lemmas.h"
#include "library/tactic/backward/backward_chaining.h"

namespace lean {
void initialize_backward_module() {
    initialize_backward_lemmas();
    initialize_backward_chaining();
}

void finalize_backward_module() {
    finalize_backward_chaining();
    finalize_backward_lemmas();
}
}
