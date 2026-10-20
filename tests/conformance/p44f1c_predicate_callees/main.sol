module conformance.p44f1c_predicate_callees

function terminal(v: Int64) -> Int64 effects { pure }
requires { v != -2 }
{
    return 8 / v
}

function helper(v: Int64) -> Bool effects { pure }
requires { v != -3 }
ensures { result == (old(v) > 0) }
{
    if v == 5 {
        return other(v - 1)
    } else {
        return terminal(v) > 0
    }
}

function other(v: Int64) -> Bool effects { pure } {
    return helper(v)
}

function core(v: Int64) -> Int64 effects { pure }
requires { helper(v) }
ensures { result == old(v) }
{
    return v
}

@entry
public function launch() -> Int64 effects { pure } {
    return core(5)
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

function text_leaf(v: Int64) -> Bool effects { pure } {
    if v == -9 {
        let first = "first dead Text constant"
        let second = "second dead Text constant"
        return first == second
    } else {
        return v > 0
    }
}

function text_helper(v: Int64) -> Bool effects { pure } {
    if v == -9 { return text_leaf(v) } else { return v > 0 }
}

function text_core(v: Int64) -> Int64 effects { pure }
requires { text_helper(v) }
{
    return v
}

public function text_root() -> Int64 effects { pure } {
    return text_core(4)
}

function mixed_outer(v: Int64) -> Bool effects { pure } {
    if v == -9 { return mixed_inner(v) } else { return v > 0 }
}

function mixed_inner(v: Int64) -> Bool effects { pure }
requires { mixed_outer(v) }
{
    return true
}

function mixed_core(v: Int64) -> Int64 effects { pure }
requires { mixed_outer(v) }
{
    return v
}

public function mixed_root() -> Int64 effects { pure } {
    return mixed_core(4)
}

function unit_leaf(v: Int64) -> () effects { pure } {
    return ()
}

function unit_helper(v: Int64) -> Bool effects { pure } {
    unit_leaf(v)
    return true
}

function unit_core(v: Int64) -> Int64 effects { pure }
requires { unit_helper(v) }
{
    return v
}

public function unit_root() -> Int64 effects { pure } {
    return unit_core(4)
}
