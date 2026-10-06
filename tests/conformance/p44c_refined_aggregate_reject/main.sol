module conformance.p44c_refined_aggregate_reject

record Pair {
    left: Int64,
    right: Int64,
}

type AnyPair = refined Pair where true

function checked(value: Pair) -> AnyPair effects { pure } {
    return AnyPair(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(Pair { left = 1, right = 2 })
    if value == value { return 42 } else { return 0 }
}
