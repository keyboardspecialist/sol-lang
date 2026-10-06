module conformance.p44c_refined_inout_reject

type Positive = refined Int64 where self > 0

function consume(value: inout Positive) -> () effects { pure } {
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = Positive(1)
    consume(value)
    return 42
}
