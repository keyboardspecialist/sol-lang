module conformance.p44c_refined_borrow_reject

type Positive = refined Int64 where self > 0

function read(value: borrow Positive) -> Int64 effects { pure } {
    return 42
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = Positive(1)
    return read(value)
}
