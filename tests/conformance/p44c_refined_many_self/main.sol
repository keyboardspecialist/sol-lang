module conformance.p44c_refined_many_self

type Positive = refined Int64 where self > 0
type Bounded = refined Int64 where self + self > self

function positive(value: Int64) -> Positive effects { pure } {
    return Positive(value)
}

function bounded(value: Int64) -> Bounded effects { pure } {
    return Bounded(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let left = positive(1)
    let right = bounded(7)
    if left == left && right == right { return 42 } else { return 0 }
}
