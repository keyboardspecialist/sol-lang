module conformance.p44c_refined_option_reject

type Positive = refined Int64 where self > 0

function wrapped(value: Int64) -> Option<Positive> effects { pure } {
    return some(Positive(value))
}

@entry
public function launch() -> Int64 effects { pure } {
    let ignored = wrapped(1)
    return 42
}
