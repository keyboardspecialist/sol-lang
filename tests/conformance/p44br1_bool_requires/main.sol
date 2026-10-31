module conformance.p44br1_bool_requires

function identity(flag: Bool) -> Bool effects { pure }
requires { true }
ensures { result == old(flag) }
{
    return flag
}

function core(flag: Bool) -> Int64 effects { pure }
requires { identity(flag) }
{
    return 7
}

@entry
public function launch() -> Int64 effects { pure } {
    let first = core(true)
    return core(true)
}

public function caller_false() -> Int64 effects { pure } {
    return core(false)
}

function literal_true_core(flag: Bool) -> Int64 effects { pure }
requires { identity(true) }
{
    return 7
}

public function literal_true() -> Int64 effects { pure } {
    return literal_true_core(false)
}

function literal_false_core(flag: Bool) -> Int64 effects { pure }
requires { identity(false) }
{
    return 7
}

public function literal_false() -> Int64 effects { pure } {
    return literal_false_core(true)
}

function requires_helper(flag: Bool) -> Bool effects { pure }
requires { false }
ensures { result == old(flag) }
{
    return flag
}

function requires_core(flag: Bool) -> Int64 effects { pure }
requires { requires_helper(flag) }
{
    return 7
}

public function callee_requires() -> Int64 effects { pure } {
    return requires_core(true)
}

function arithmetic_helper(flag: Bool) -> Bool effects { pure }
requires { true }
ensures { result == old(flag) }
{
    if flag {
        return flag
    } else {
        let quotient = 8 / 0
        return quotient > 0
    }
}

function arithmetic_core(flag: Bool) -> Int64 effects { pure }
requires { arithmetic_helper(flag) }
{
    return 7
}

public function callee_arithmetic() -> Int64 effects { pure } {
    return arithmetic_core(false)
}

function ensures_helper(flag: Bool) -> Bool effects { pure }
requires { true }
ensures { result == old(flag) }
{
    return !flag
}

function ensures_core(flag: Bool) -> Int64 effects { pure }
requires { ensures_helper(flag) }
{
    return 7
}

public function callee_ensures() -> Int64 effects { pure } {
    return ensures_core(true)
}

function reject_wrapper(flag: Bool) -> Int64 effects { pure }
requires { identity(flag) == true }
{
    return 7
}

function reject_computed_argument(flag: Bool) -> Int64 effects { pure }
requires { identity(!flag) }
{
    return 7
}

function reject_nested_invoke(flag: Bool) -> Int64 effects { pure }
requires { identity(identity(flag)) }
{
    return 7
}

function two_arguments(left: Bool, right: Bool) -> Bool effects { pure } {
    return left == right
}

function reject_arity(flag: Bool) -> Int64 effects { pure }
requires { two_arguments(flag, true) }
{
    return 7
}

function unit_helper(value: ()) -> Bool effects { pure } {
    return true
}

function reject_unit_parameter(flag: Bool) -> Int64 effects { pure }
requires { unit_helper(()) }
{
    return 7
}

function as_int(flag: Bool) -> Int64 effects { pure } {
    if flag { return 1 } else { return 0 }
}

function reject_nonbool_result(flag: Bool) -> Int64 effects { pure }
requires { as_int(flag) == 1 }
{
    return 7
}

function reject_bool_ensures(flag: Bool) -> Bool effects { pure }
ensures { identity(result) }
{
    return flag
}

function dead_text_target(flag: Bool) -> Bool effects { pure } {
    if flag {
        return flag
    } else {
        let dead = "dead owned rich value"
        return flag
    }
}

function reject_target_dead_text(flag: Bool) -> Int64 effects { pure }
requires { dead_text_target(flag) }
{
    return 7
}

function ordinary_target(flag: Bool) -> Bool effects { pure } {
    if flag { return flag } else { return identity(flag) }
}

function reject_target_ordinary_call(flag: Bool) -> Int64 effects { pure }
requires { ordinary_target(flag) }
{
    return 7
}

function predicate_target(flag: Bool) -> Bool effects { pure }
requires { identity(flag) }
{
    return flag
}

function reject_target_predicate_call(flag: Bool) -> Int64 effects { pure }
requires { predicate_target(flag) }
{
    return 7
}

function reject_guard(flag: Bool) -> Int64 effects { pure }
requires { identity(flag) }
{
    return match flag {
        true if identity(flag) => 7
        _ => 0
    }
}
