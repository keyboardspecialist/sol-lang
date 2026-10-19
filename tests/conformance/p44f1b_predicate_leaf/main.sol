module conformance.p44f1b_predicate_leaf

function helper(v: Int64) -> Bool effects { pure }
requires { v != -2 }
ensures { result == (old(v) > 0) }
{
    return (8 / v) > 0
}

function core(v: Int64) -> Int64 effects { pure }
requires { helper(v) }
ensures { result == old(v) }
{
    return v
}

@entry
public function launch() -> Int64 effects { pure } {
    let first = core(4)
    return core(4)
}

public function caller_false() -> Int64 effects { pure } {
    return core(-1)
}

public function callee_requires() -> Int64 effects { pure } {
    return core(-2)
}

public function callee_arithmetic() -> Int64 effects { pure } {
    return core(0)
}

public function callee_ensures() -> Int64 effects { pure } {
    return core(20)
}
