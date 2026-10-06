module conformance.p44c_refined_result_reject

enum Error { failed }
type Positive = refined Int64 where self > 0

function wrapped(value: Int64) -> Result<Positive, Error> effects { pure } {
    return ok(Positive(value))
}

@entry
public function launch() -> Int64 effects { pure } {
    let ignored = wrapped(1)
    return 42
}
