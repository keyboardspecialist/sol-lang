module conformance.p44c_refined_indirect_reject

type Positive = refined Int64 where self > 0

function identity(value: Positive) -> Positive effects { pure } {
    return value
}

@entry
public function launch() -> Int64 effects { pure } {
    let callback = identity
    let value = callback(Positive(1))
    if value == value { return 42 } else { return 0 }
}
