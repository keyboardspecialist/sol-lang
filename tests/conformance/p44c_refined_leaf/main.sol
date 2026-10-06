module conformance.p44c_refined_leaf

type Positive = refined Int64 where self > 0
type Enabled = refined Bool where self
type Always = refined Int64 where true
type BranchedArithmetic = refined Int64 where if self == 0 {
    12 / self > 0
} else {
    self > 0
}

function positive(value: Int64) -> Positive effects { pure } {
    return Positive(value)
}

function enabled(value: Bool) -> Enabled effects { pure } {
    return Enabled(value)
}

function always(value: Int64) -> Always effects { pure } {
    return Always(value)
}

function branched(value: Int64) -> BranchedArithmetic effects { pure } {
    return BranchedArithmetic(value)
}

function bounce(value: Positive) -> Positive effects { pure } {
    return value
}

function same(left: Positive, right: Positive) -> Bool effects { pure } {
    return left == right
}

@entry
public function launch_true() -> Int64 effects { pure } {
    let value = positive(1)
    let moved = bounce(value)
    if same(moved, moved) { return 42 } else { return 0 }
}

public function launch_false() -> Int64 effects { pure } {
    let value = positive(0)
    if same(value, value) { return 42 } else { return 0 }
}

public function launch_bool() -> Int64 effects { pure } {
    let left = enabled(true)
    let right = enabled(true)
    if left == right { return 1 } else { return 0 }
}

public function launch_constant() -> Int64 effects { pure } {
    let value = always(-1)
    if value == value { return 7 } else { return 0 }
}

public function launch_arithmetic() -> Int64 effects { pure } {
    let value = branched(0)
    if value == value { return 42 } else { return 0 }
}
