module conformance.p44f1d_refinement_calls

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

type Accepted = refined Int64 where helper(self)
type Wrapped = refined Int64 where helper(self) == true
type Computed = refined Int64 where helper(self - 1)
type BoolRefined = refined Bool where bool_helper(self)
type Nested = refined Int64 where nested_target(self)
type Target = refined Int64 where target_refinement(self)
type DeadText = refined Int64 where target_dead_text(self)

function core(v: Int64) -> Int64 effects { pure }
requires { v != -4 }
ensures { result == old(v) }
{
    let checked = Accepted(v)
    return v
}

@entry
public function launch() -> Int64 effects { pure } {
    return core(5)
}

public function caller_requires() -> Int64 effects { pure } {
    return core(-4)
}

public function refined_false() -> Int64 effects { pure } {
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

function wrapper_type() -> Int64 effects { pure } {
    let rejected = Wrapped(4)
    return 4
}

function computed_self_type() -> Int64 effects { pure } {
    let rejected = Computed(4)
    return 4
}

function bool_helper(v: Bool) -> Bool effects { pure } {
    return v
}

function bool_self_type() -> Int64 effects { pure } {
    let rejected = BoolRefined(true)
    return 4
}

function ensures_call(v: Int64) -> Int64 effects { pure }
ensures { helper(result) }
{
    return v
}

function ensures_root() -> Int64 effects { pure } {
    return ensures_call(4)
}

function nested_target(v: Int64) -> Bool effects { pure }
requires { helper(v) }
{
    return v > 0
}

function nested_predicate_type() -> Int64 effects { pure } {
    let rejected = Nested(4)
    return 4
}

function target_refinement(v: Int64) -> Bool effects { pure } {
    let nested = Accepted(4)
    return true
}

function target_refinement_type() -> Int64 effects { pure } {
    let rejected = Target(4)
    return 4
}

function target_dead_text(v: Int64) -> Bool effects { pure } {
    if v == -9 {
        let dead = "dead Text target path"
        return v > 0
    } else {
        return v > 0
    }
}

function target_dead_text_type() -> Int64 effects { pure } {
    let rejected = DeadText(4)
    return 4
}
