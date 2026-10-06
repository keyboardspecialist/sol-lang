module conformance.p44c_refined_nested_reject

type Positive = refined Int64 where self > 0
type Nested = refined Positive where true

function nested(value: Positive) -> Nested effects { pure } {
    return Nested(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = nested(Positive(1))
    if value == value { return 42 } else { return 0 }
}
