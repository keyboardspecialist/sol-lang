module conformance.p44c_refined_seal

type Positive = refined Int64 where self > 0

function positive(value: Int64) -> Positive effects { pure } {
    return Positive(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = positive(1)
    if value == value { return 42 } else { return 0 }
}
