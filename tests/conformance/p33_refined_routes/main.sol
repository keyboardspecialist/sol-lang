module conformance.p33_refined_routes

type Positive = refined Int64 where self > 0

type BranchedArithmetic = refined Int64 where if self == 0 {
    12 / self > 0
} else {
    self > 0
}

function direct_true() -> Positive effects { pure } {
    return Positive(1)
}

function direct_false() -> Positive effects { pure } {
    return Positive(0)
}

function post_branch_true(choose: Bool) -> Positive effects { pure } {
    let value = if choose { 1 } else { 2 }
    return Positive(value)
}

function post_branch_false(choose: Bool) -> Positive effects { pure } {
    let value = if choose { 0 } else { -1 }
    return Positive(value)
}

function arithmetic_after_branch(value: Int64) -> BranchedArithmetic effects { pure } {
    return BranchedArithmetic(value)
}
