module conformance.p44c_refined_arithmetic

type BranchedArithmetic = refined Int64 where if self == 0 {
    12 / self > 0
} else {
    self > 0
}

function branched(value: Int64) -> BranchedArithmetic effects { pure } {
    return BranchedArithmetic(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = branched(0)
    if value == value { return 42 } else { return 0 }
}
