module conformance.p44c_refined_overflow

type Incrementable = refined Int64 where self + 1 > self

function checked(value: Int64) -> Incrementable effects { pure } {
    return Incrementable(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(9223372036854775807)
    if value == value { return 42 } else { return 0 }
}
