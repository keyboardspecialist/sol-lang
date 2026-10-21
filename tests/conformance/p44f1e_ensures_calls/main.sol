module conformance.p44f1e_ensures_calls

enum Failure { failed }

function terminal(v: Int64) -> Int64 effects { pure } {
    return 8 / v
}

function helper(v: Int64) -> Bool effects { pure }
requires { v != -2 }
ensures { result == (old(v) > 0) }
{
    if v == 5 {
        return helper(v - 1)
    } else {
        return terminal(v) > 0
    }
}

function core(v: Int64) -> Int64 effects { pure }
requires { v != -4 }
ensures { helper(result) }
ensures { helper(old(v)) }
{
    return v - 1
}

function old_core(v: Int64) -> Int64 effects { pure }
requires { v != -4 }
ensures { helper(old(v)) }
{
    return 4
}

@entry
public function launch() -> Int64 effects { pure } {
    return core(6)
}

public function caller_requires() -> Int64 effects { pure } {
    return core(-4)
}

public function caller_false() -> Int64 effects { pure } {
    return core(0)
}

public function callee_requires() -> Int64 effects { pure } {
    return core(-1)
}

public function callee_arithmetic() -> Int64 effects { pure } {
    return core(1)
}

public function callee_ensures() -> Int64 effects { pure } {
    return core(21)
}

public function old_false() -> Int64 effects { pure } {
    return old_core(-1)
}

public function old_callee_arithmetic() -> Int64 effects { pure } {
    return old_core(0)
}

function reject_wrapper(v: Int64) -> Int64 effects { pure }
ensures { helper(result) == true }
{
    return v
}

function reject_computed_argument(v: Int64) -> Int64 effects { pure }
ensures { helper(result - 1) }
{
    return v
}

function reject_parameter_argument(v: Int64) -> Int64 effects { pure }
ensures { helper(v) }
{
    return v
}

function reject_qualified(v: Int64) -> Result<Int64, Failure> effects { pure }
ensures { success => helper(result) }
{
    return ok(v)
}

function bool_helper(v: Bool) -> Bool effects { pure } {
    return v
}

function reject_bool_argument(v: Bool) -> Bool effects { pure }
ensures { bool_helper(result) }
{
    return v
}

function nested_target(v: Int64) -> Bool effects { pure }
requires { helper(v) }
{
    return v > 0
}

function reject_nested_predicate(v: Int64) -> Int64 effects { pure }
ensures { nested_target(result) }
{
    return v
}

type Accepted = refined Int64 where self > 0

function target_refinement(v: Int64) -> Bool effects { pure } {
    let nested = Accepted(4)
    return true
}

function reject_target_refinement(v: Int64) -> Int64 effects { pure }
ensures { target_refinement(result) }
{
    return v
}

function target_dead_text(v: Int64) -> Bool effects { pure } {
    if v == -9 {
        let dead = "dead Text target path"
        return v > 0
    } else {
        return v > 0
    }
}

function reject_target_dead_text(v: Int64) -> Int64 effects { pure }
ensures { target_dead_text(result) }
{
    return v
}
